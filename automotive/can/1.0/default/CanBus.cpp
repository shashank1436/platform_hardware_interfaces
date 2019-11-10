/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "CanBus.h"

#include "CloseHandle.h"

#include <android-base/logging.h>
#include <libnetdevice/can.h>
#include <libnetdevice/libnetdevice.h>
#include <linux/can.h>

namespace android {
namespace hardware {
namespace automotive {
namespace can {
namespace V1_0 {
namespace implementation {

/**
 * Whether to log sent/received packets.
 */
static constexpr bool kSuperVerbose = false;

Return<Result> CanBus::send(const CanMessage& message) {
    std::lock_guard<std::mutex> lck(mIsUpGuard);
    if (!mIsUp) return Result::INTERFACE_DOWN;

    if (UNLIKELY(kSuperVerbose)) {
        LOG(VERBOSE) << "Sending " << toString(message);
    }

    if (message.payload.size() > CAN_MAX_DLEN) return Result::PAYLOAD_TOO_LONG;

    struct canfd_frame frame = {};
    frame.can_id = message.id;
    frame.len = message.payload.size();
    memcpy(frame.data, message.payload.data(), message.payload.size());

    if (!mSocket->send(frame)) return Result::TRANSMISSION_FAILURE;

    return Result::OK;
}

Return<void> CanBus::listen(const hidl_vec<CanMessageFilter>& filter,
                            const sp<ICanMessageListener>& listenerCb, listen_cb _hidl_cb) {
    std::lock_guard<std::mutex> lck(mIsUpGuard);

    if (listenerCb == nullptr) {
        _hidl_cb(Result::INVALID_ARGUMENTS, nullptr);
        return {};
    }
    if (!mIsUp) {
        _hidl_cb(Result::INTERFACE_DOWN, nullptr);
        return {};
    }

    std::lock_guard<std::mutex> lckListeners(mMsgListenersGuard);

    sp<CloseHandle> closeHandle = new CloseHandle([this, listenerCb]() {
        std::lock_guard<std::mutex> lck(mMsgListenersGuard);
        std::erase_if(mMsgListeners, [&](const auto& e) { return e.callback == listenerCb; });
    });
    mMsgListeners.emplace_back(CanMessageListener{listenerCb, filter, closeHandle});
    auto& listener = mMsgListeners.back();

    // fix message IDs to have all zeros on bits not covered by mask
    std::for_each(listener.filter.begin(), listener.filter.end(),
                  [](auto& rule) { rule.id &= rule.mask; });

    _hidl_cb(Result::OK, closeHandle);
    return {};
}

CanBus::CanBus(const std::string& ifname) : mIfname(ifname) {}

CanBus::~CanBus() {
    std::lock_guard<std::mutex> lck(mIsUpGuard);
    CHECK(!mIsUp) << "Interface is still up while being destroyed";

    std::lock_guard<std::mutex> lckListeners(mMsgListenersGuard);
    CHECK(mMsgListeners.empty()) << "Listener list is not empty while interface is being destroyed";
}

void CanBus::setErrorCallback(ErrorCallback errcb) {
    CHECK(!mIsUp) << "Can't set error callback while interface is up";
    CHECK(mErrCb == nullptr) << "Error callback is already set";
    mErrCb = errcb;
    CHECK(!mIsUp) << "Can't set error callback while interface is up";
}

ICanController::Result CanBus::preUp() {
    return ICanController::Result::OK;
}

bool CanBus::postDown() {
    return true;
}

ICanController::Result CanBus::up() {
    std::lock_guard<std::mutex> lck(mIsUpGuard);

    if (mIsUp) {
        LOG(WARNING) << "Interface is already up";
        return ICanController::Result::INVALID_STATE;
    }

    const auto preResult = preUp();
    if (preResult != ICanController::Result::OK) return preResult;

    const auto isUp = netdevice::isUp(mIfname);
    if (!isUp.has_value()) {
        // preUp() should prepare the interface (either create or make sure it's there)
        LOG(ERROR) << "Interface " << mIfname << " didn't get prepared";
        return ICanController::Result::BAD_ADDRESS;
    }

    if (!*isUp && !netdevice::up(mIfname)) {
        LOG(ERROR) << "Can't bring " << mIfname << " up";
        return ICanController::Result::UNKNOWN_ERROR;
    }
    mDownAfterUse = !*isUp;

    using namespace std::placeholders;
    CanSocket::ReadCallback rdcb = std::bind(&CanBus::onRead, this, _1, _2);
    CanSocket::ErrorCallback errcb = std::bind(&CanBus::onError, this, _1);
    mSocket = CanSocket::open(mIfname, rdcb, errcb);
    if (!mSocket) {
        if (mDownAfterUse) netdevice::down(mIfname);
        return ICanController::Result::UNKNOWN_ERROR;
    }

    mIsUp = true;
    return ICanController::Result::OK;
}

void CanBus::clearMsgListeners() {
    std::vector<wp<ICloseHandle>> listenersToClose;
    {
        std::lock_guard<std::mutex> lck(mMsgListenersGuard);
        std::transform(mMsgListeners.begin(), mMsgListeners.end(),
                       std::back_inserter(listenersToClose),
                       [](const auto& e) { return e.closeHandle; });
    }

    for (auto& weakListener : listenersToClose) {
        /* Between populating listenersToClose and calling close method here, some listeners might
         * have been already removed from the original mMsgListeners list (resulting in a dangling
         * weak pointer here). It's fine - we just want to clean them up. */
        auto listener = weakListener.promote();
        if (listener != nullptr) listener->close();
    }

    std::lock_guard<std::mutex> lck(mMsgListenersGuard);
    CHECK(mMsgListeners.empty()) << "Listeners list wasn't emptied";
}

void CanBus::clearErrListeners() {
    std::lock_guard<std::mutex> lck(mErrListenersGuard);
    mErrListeners.clear();
}

Return<sp<ICloseHandle>> CanBus::listenForErrors(const sp<ICanErrorListener>& listener) {
    if (listener == nullptr) {
        return new CloseHandle();
    }

    std::lock_guard<std::mutex> upLck(mIsUpGuard);
    if (!mIsUp) {
        listener->onError(ErrorEvent::INTERFACE_DOWN, true);
        return new CloseHandle();
    }

    std::lock_guard<std::mutex> errLck(mErrListenersGuard);
    mErrListeners.emplace_back(listener);

    return new CloseHandle([this, listener]() {
        std::lock_guard<std::mutex> lck(mErrListenersGuard);
        std::erase(mErrListeners, listener);
    });
}

bool CanBus::down() {
    std::lock_guard<std::mutex> lck(mIsUpGuard);

    if (!mIsUp) {
        LOG(WARNING) << "Interface is already down";
        return false;
    }
    mIsUp = false;

    clearMsgListeners();
    clearErrListeners();
    mSocket.reset();

    bool success = true;

    if (mDownAfterUse && !netdevice::down(mIfname)) {
        LOG(ERROR) << "Can't bring " << mIfname << " down";
        // don't return yet, let's try to do best-effort cleanup
        success = false;
    }

    if (!postDown()) success = false;

    return success;
}

/**
 * Match the filter set against message id.
 *
 * For details on the filters syntax, please see CanMessageFilter at
 * the HAL definition (types.hal).
 *
 * \param filter Filter to match against
 * \param id Message id to filter
 * \return true if the message id matches the filter, false otherwise
 */
static bool match(const hidl_vec<CanMessageFilter>& filter, CanMessageId id) {
    if (filter.size() == 0) return true;

    bool anyNonInvertedPresent = false;
    bool anyNonInvertedSatisfied = false;
    for (auto& rule : filter) {
        const bool satisfied = ((id & rule.mask) == rule.id) == !rule.inverted;
        if (rule.inverted) {
            // Any inverted (blacklist) rule not being satisfied invalidates the whole filter set.
            if (!satisfied) return false;
        } else {
            anyNonInvertedPresent = true;
            if (satisfied) anyNonInvertedSatisfied = true;
        }
    }
    return !anyNonInvertedPresent || anyNonInvertedSatisfied;
}

void CanBus::onRead(const struct canfd_frame& frame, std::chrono::nanoseconds timestamp) {
    CanMessage message = {};
    message.id = frame.can_id;
    message.payload = hidl_vec<uint8_t>(frame.data, frame.data + frame.len);
    message.timestamp = timestamp.count();

    if (UNLIKELY(kSuperVerbose)) {
        LOG(VERBOSE) << "Got message " << toString(message);
    }

    std::lock_guard<std::mutex> lck(mMsgListenersGuard);
    for (auto& listener : mMsgListeners) {
        if (!match(listener.filter, message.id)) continue;
        if (!listener.callback->onReceive(message).isOk() && !listener.failedOnce) {
            listener.failedOnce = true;
            LOG(WARNING) << "Failed to notify listener about message";
        }
    }
}

void CanBus::onError(int errnoVal) {
    auto eventType = ErrorEvent::HARDWARE_ERROR;

    if (errnoVal == ENODEV || errnoVal == ENETDOWN) {
        mDownAfterUse = false;
        eventType = ErrorEvent::INTERFACE_DOWN;
    }

    {
        std::lock_guard<std::mutex> lck(mErrListenersGuard);
        for (auto& listener : mErrListeners) {
            if (!listener->onError(eventType, true).isOk()) {
                LOG(WARNING) << "Failed to notify listener about error";
            }
        }
    }

    const auto errcb = mErrCb;
    if (errcb != nullptr) errcb();
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace can
}  // namespace automotive
}  // namespace hardware
}  // namespace android