/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "ExtCamDev@3.4"
#define LOG_NDEBUG 0
#include <log/log.h>

#include <array>
#include <linux/videodev2.h>
#include "android-base/macros.h"
#include "CameraMetadata.h"
#include "../../3.2/default/include/convert.h"
#include "ExternalCameraDevice_3_4.h"


namespace android {
namespace hardware {
namespace camera {
namespace device {
namespace V3_4 {
namespace implementation {

namespace {
// Only support MJPEG for now as it seems to be the one supports higher fps
// Other formats to consider in the future:
// * V4L2_PIX_FMT_YVU420 (== YV12)
// * V4L2_PIX_FMT_YVYU (YVYU: can be converted to YV12 or other YUV420_888 formats)
const std::array<uint32_t, /*size*/1> kSupportedFourCCs {{
    V4L2_PIX_FMT_MJPEG
}}; // double braces required in C++11

// TODO: b/72261897
//       Define max size/fps this Android device can advertise (and streaming at reasonable speed)
//       Also make sure that can be done without editing source code

// TODO: b/72261675: make it dynamic since this affects memory usage
const int kMaxJpegSize = {13 * 1024 * 1024};  // 13MB
} // anonymous namespace

ExternalCameraDevice::ExternalCameraDevice(const std::string& cameraId) :
        mCameraId(cameraId) {
    status_t ret = initCameraCharacteristics();
    if (ret != OK) {
        ALOGE("%s: init camera characteristics failed: errorno %d", __FUNCTION__, ret);
        mInitFailed = true;
    }
}

ExternalCameraDevice::~ExternalCameraDevice() {}

bool ExternalCameraDevice::isInitFailed() {
    return mInitFailed;
}

Return<void> ExternalCameraDevice::getResourceCost(getResourceCost_cb _hidl_cb) {
    CameraResourceCost resCost;
    resCost.resourceCost = 100;
    _hidl_cb(Status::OK, resCost);
    return Void();
}

Return<void> ExternalCameraDevice::getCameraCharacteristics(
        getCameraCharacteristics_cb _hidl_cb) {
    Mutex::Autolock _l(mLock);
    V3_2::CameraMetadata hidlChars;

    if (isInitFailed()) {
        _hidl_cb(Status::INTERNAL_ERROR, hidlChars);
        return Void();
    }

    const camera_metadata_t* rawMetadata = mCameraCharacteristics.getAndLock();
    V3_2::implementation::convertToHidl(rawMetadata, &hidlChars);
    _hidl_cb(Status::OK, hidlChars);
    mCameraCharacteristics.unlock(rawMetadata);
    return Void();
}

Return<Status> ExternalCameraDevice::setTorchMode(TorchMode) {
    return Status::METHOD_NOT_SUPPORTED;
}

Return<void> ExternalCameraDevice::open(
        const sp<ICameraDeviceCallback>& callback, open_cb _hidl_cb) {
    Status status = Status::OK;
    sp<ExternalCameraDeviceSession> session = nullptr;

    if (callback == nullptr) {
        ALOGE("%s: cannot open camera %s. callback is null!",
                __FUNCTION__, mCameraId.c_str());
        _hidl_cb(Status::ILLEGAL_ARGUMENT, nullptr);
        return Void();
    }

    if (isInitFailed()) {
        ALOGE("%s: cannot open camera %s. camera init failed!",
                __FUNCTION__, mCameraId.c_str());
        _hidl_cb(Status::INTERNAL_ERROR, nullptr);
        return Void();
    }

    mLock.lock();

    ALOGV("%s: Initializing device for camera %s", __FUNCTION__, mCameraId.c_str());
    session = mSession.promote();
    if (session != nullptr && !session->isClosed()) {
        ALOGE("%s: cannot open an already opened camera!", __FUNCTION__);
        mLock.unlock();
        _hidl_cb(Status::CAMERA_IN_USE, nullptr);
        return Void();
    }

    unique_fd fd(::open(mCameraId.c_str(), O_RDWR));
    if (fd.get() < 0) {
        ALOGE("%s: v4l2 device open %s failed: %s",
                __FUNCTION__, mCameraId.c_str(), strerror(errno));
        mLock.unlock();
        _hidl_cb(Status::INTERNAL_ERROR, nullptr);
        return Void();
    }

    session = new ExternalCameraDeviceSession(
            callback, mSupportedFormats, mCameraCharacteristics, std::move(fd));
    if (session == nullptr) {
        ALOGE("%s: camera device session allocation failed", __FUNCTION__);
        mLock.unlock();
        _hidl_cb(Status::INTERNAL_ERROR, nullptr);
        return Void();
    }
    if (session->isInitFailed()) {
        ALOGE("%s: camera device session init failed", __FUNCTION__);
        session = nullptr;
        mLock.unlock();
        _hidl_cb(Status::INTERNAL_ERROR, nullptr);
        return Void();
    }
    mSession = session;

    mLock.unlock();

    _hidl_cb(status, session->getInterface());
    return Void();
}

Return<void> ExternalCameraDevice::dumpState(const ::android::hardware::hidl_handle& handle) {
    Mutex::Autolock _l(mLock);
    if (handle.getNativeHandle() == nullptr) {
        ALOGE("%s: handle must not be null", __FUNCTION__);
        return Void();
    }
    if (handle->numFds != 1 || handle->numInts != 0) {
        ALOGE("%s: handle must contain 1 FD and 0 integers! Got %d FDs and %d ints",
                __FUNCTION__, handle->numFds, handle->numInts);
        return Void();
    }
    int fd = handle->data[0];
    if (mSession == nullptr) {
        dprintf(fd, "No active camera device session instance\n");
        return Void();
    }
    auto session = mSession.promote();
    if (session == nullptr) {
        dprintf(fd, "No active camera device session instance\n");
        return Void();
    }
    // Call into active session to dump states
    session->dumpState(handle);
    return Void();
}


status_t ExternalCameraDevice::initCameraCharacteristics() {
    if (mCameraCharacteristics.isEmpty()) {
        // init camera characteristics
        unique_fd fd(::open(mCameraId.c_str(), O_RDWR));
        if (fd.get() < 0) {
            ALOGE("%s: v4l2 device open %s failed", __FUNCTION__, mCameraId.c_str());
            return DEAD_OBJECT;
        }

        status_t ret;
        ret = initDefaultCharsKeys(&mCameraCharacteristics);
        if (ret != OK) {
            ALOGE("%s: init default characteristics key failed: errorno %d", __FUNCTION__, ret);
            mCameraCharacteristics.clear();
            return ret;
        }

        ret = initCameraControlsCharsKeys(fd.get(), &mCameraCharacteristics);
        if (ret != OK) {
            ALOGE("%s: init camera control characteristics key failed: errorno %d", __FUNCTION__, ret);
            mCameraCharacteristics.clear();
            return ret;
        }

        ret = initOutputCharsKeys(fd.get(), &mCameraCharacteristics);
        if (ret != OK) {
            ALOGE("%s: init output characteristics key failed: errorno %d", __FUNCTION__, ret);
            mCameraCharacteristics.clear();
            return ret;
        }
    }
    return OK;
}

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#define UPDATE(tag, data, size)                    \
do {                                               \
  if (metadata->update((tag), (data), (size))) {   \
    ALOGE("Update " #tag " failed!");              \
    return -EINVAL;                                \
  }                                                \
} while (0)

status_t ExternalCameraDevice::initDefaultCharsKeys(
        ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata) {
    const uint8_t hardware_level = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL;
    UPDATE(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hardware_level, 1);

    // android.colorCorrection
    const uint8_t availableAberrationModes[] = {
        ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF};
    UPDATE(ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
           availableAberrationModes, ARRAY_SIZE(availableAberrationModes));

    // android.control
    const uint8_t antibandingMode =
        ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO;
    UPDATE(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
           &antibandingMode, 1);

    const int32_t controlMaxRegions[] = {/*AE*/ 0, /*AWB*/ 0, /*AF*/ 0};
    UPDATE(ANDROID_CONTROL_MAX_REGIONS, controlMaxRegions,
           ARRAY_SIZE(controlMaxRegions));

    const uint8_t videoStabilizationMode =
        ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    UPDATE(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
           &videoStabilizationMode, 1);

    const uint8_t awbAvailableMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    UPDATE(ANDROID_CONTROL_AWB_AVAILABLE_MODES, &awbAvailableMode, 1);

    const uint8_t aeAvailableMode = ANDROID_CONTROL_AE_MODE_ON;
    UPDATE(ANDROID_CONTROL_AE_AVAILABLE_MODES, &aeAvailableMode, 1);

    const uint8_t availableFffect = ANDROID_CONTROL_EFFECT_MODE_OFF;
    UPDATE(ANDROID_CONTROL_AVAILABLE_EFFECTS, &availableFffect, 1);

    const uint8_t controlAvailableModes[] = {ANDROID_CONTROL_MODE_OFF,
                                             ANDROID_CONTROL_MODE_AUTO};
    UPDATE(ANDROID_CONTROL_AVAILABLE_MODES, controlAvailableModes,
           ARRAY_SIZE(controlAvailableModes));

    // android.edge
    const uint8_t edgeMode = ANDROID_EDGE_MODE_OFF;
    UPDATE(ANDROID_EDGE_AVAILABLE_EDGE_MODES, &edgeMode, 1);

    // android.flash
    const uint8_t flashInfo = ANDROID_FLASH_INFO_AVAILABLE_FALSE;
    UPDATE(ANDROID_FLASH_INFO_AVAILABLE, &flashInfo, 1);

    // android.hotPixel
    const uint8_t hotPixelMode = ANDROID_HOT_PIXEL_MODE_OFF;
    UPDATE(ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES, &hotPixelMode, 1);

    // android.jpeg
    // TODO: b/72261675 See if we can provide thumbnail size for all jpeg aspect ratios
    const int32_t jpegAvailableThumbnailSizes[] = {0, 0, 240, 180};
    UPDATE(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, jpegAvailableThumbnailSizes,
           ARRAY_SIZE(jpegAvailableThumbnailSizes));

    const int32_t jpegMaxSize = kMaxJpegSize;
    UPDATE(ANDROID_JPEG_MAX_SIZE, &jpegMaxSize, 1);

    const uint8_t jpegQuality = 90;
    UPDATE(ANDROID_JPEG_QUALITY, &jpegQuality, 1);
    UPDATE(ANDROID_JPEG_THUMBNAIL_QUALITY, &jpegQuality, 1);

    const int32_t jpegOrientation = 0;
    UPDATE(ANDROID_JPEG_ORIENTATION, &jpegOrientation, 1);

    // android.lens
    const uint8_t focusDistanceCalibration =
            ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED;
    UPDATE(ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &focusDistanceCalibration, 1);

    const uint8_t opticalStabilizationMode =
        ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
    UPDATE(ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
           &opticalStabilizationMode, 1);

    const uint8_t facing = ANDROID_LENS_FACING_EXTERNAL;
    UPDATE(ANDROID_LENS_FACING, &facing, 1);

    // android.noiseReduction
    const uint8_t noiseReductionMode = ANDROID_NOISE_REDUCTION_MODE_OFF;
    UPDATE(ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
           &noiseReductionMode, 1);
    UPDATE(ANDROID_NOISE_REDUCTION_MODE, &noiseReductionMode, 1);

    // android.request
    const uint8_t availableCapabilities[] = {
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE};
    UPDATE(ANDROID_REQUEST_AVAILABLE_CAPABILITIES, availableCapabilities,
           ARRAY_SIZE(availableCapabilities));

    const int32_t partialResultCount = 1;
    UPDATE(ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialResultCount, 1);

    // This means pipeline latency of X frame intervals. The maximum number is 4.
    const uint8_t requestPipelineMaxDepth = 4;
    UPDATE(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &requestPipelineMaxDepth, 1);
    UPDATE(ANDROID_REQUEST_PIPELINE_DEPTH, &requestPipelineMaxDepth, 1);

    // Three numbers represent the maximum numbers of different types of output
    // streams simultaneously. The types are raw sensor, processed (but not
    // stalling), and processed (but stalling). For usb limited mode, raw sensor
    // is not supported. Stalling stream is JPEG. Non-stalling streams are
    // YUV_420_888 or YV12.
    const int32_t requestMaxNumOutputStreams[] = {
            /*RAW*/0,
            /*Processed*/ExternalCameraDeviceSession::kMaxProcessedStream,
            /*Stall*/ExternalCameraDeviceSession::kMaxStallStream};
    UPDATE(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, requestMaxNumOutputStreams,
           ARRAY_SIZE(requestMaxNumOutputStreams));

    // Limited mode doesn't support reprocessing.
    const int32_t requestMaxNumInputStreams = 0;
    UPDATE(ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS, &requestMaxNumInputStreams,
           1);

    // android.scaler
    // TODO: b/72263447 V4L2_CID_ZOOM_*
    const float scalerAvailableMaxDigitalZoom[] = {1};
    UPDATE(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
           scalerAvailableMaxDigitalZoom,
           ARRAY_SIZE(scalerAvailableMaxDigitalZoom));

    const uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY;
    UPDATE(ANDROID_SCALER_CROPPING_TYPE, &croppingType, 1);

    const int32_t testPatternModes[] = {
        ANDROID_SENSOR_TEST_PATTERN_MODE_OFF};
    UPDATE(ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES, testPatternModes,
           ARRAY_SIZE(testPatternModes));
    UPDATE(ANDROID_SENSOR_TEST_PATTERN_MODE, &testPatternModes[0], 1);

    const uint8_t timestampSource = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN;
    UPDATE(ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &timestampSource, 1);

    // Orientation probably isn't useful for external facing camera?
    const int32_t orientation = 0;
    UPDATE(ANDROID_SENSOR_ORIENTATION, &orientation, 1);

    // android.shading
    const uint8_t availabeMode = ANDROID_SHADING_MODE_OFF;
    UPDATE(ANDROID_SHADING_AVAILABLE_MODES, &availabeMode, 1);

    // android.statistics
    const uint8_t faceDetectMode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
    UPDATE(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, &faceDetectMode,
           1);

    const int32_t maxFaceCount = 0;
    UPDATE(ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &maxFaceCount, 1);

    const uint8_t availableHotpixelMode =
        ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF;
    UPDATE(ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
           &availableHotpixelMode, 1);

    const uint8_t lensShadingMapMode =
        ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF;
    UPDATE(ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
           &lensShadingMapMode, 1);

    // android.sync
    const int32_t maxLatency = ANDROID_SYNC_MAX_LATENCY_UNKNOWN;
    UPDATE(ANDROID_SYNC_MAX_LATENCY, &maxLatency, 1);

    /* Other sensor/RAW realted keys:
     * android.sensor.info.colorFilterArrangement -> no need if we don't do RAW
     * android.sensor.info.physicalSize           -> not available
     * android.sensor.info.whiteLevel             -> not available/not needed
     * android.sensor.info.lensShadingApplied     -> not needed
     * android.sensor.info.preCorrectionActiveArraySize -> not available/not needed
     * android.sensor.blackLevelPattern           -> not available/not needed
     */

    const int32_t availableRequestKeys[] = {
        ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
        ANDROID_CONTROL_AE_ANTIBANDING_MODE,
        ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
        ANDROID_CONTROL_AE_LOCK,
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
        ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
        ANDROID_CONTROL_AF_MODE,
        ANDROID_CONTROL_AF_TRIGGER,
        ANDROID_CONTROL_AWB_LOCK,
        ANDROID_CONTROL_AWB_MODE,
        ANDROID_CONTROL_CAPTURE_INTENT,
        ANDROID_CONTROL_EFFECT_MODE,
        ANDROID_CONTROL_MODE,
        ANDROID_CONTROL_SCENE_MODE,
        ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
        ANDROID_FLASH_MODE,
        ANDROID_JPEG_ORIENTATION,
        ANDROID_JPEG_QUALITY,
        ANDROID_JPEG_THUMBNAIL_QUALITY,
        ANDROID_JPEG_THUMBNAIL_SIZE,
        ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
        ANDROID_NOISE_REDUCTION_MODE,
        ANDROID_SCALER_CROP_REGION,
        ANDROID_SENSOR_TEST_PATTERN_MODE,
        ANDROID_STATISTICS_FACE_DETECT_MODE,
        ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE};
    UPDATE(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, availableRequestKeys,
           ARRAY_SIZE(availableRequestKeys));

    const int32_t availableResultKeys[] = {
        ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
        ANDROID_CONTROL_AE_ANTIBANDING_MODE,
        ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
        ANDROID_CONTROL_AE_LOCK,
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
        ANDROID_CONTROL_AE_STATE,
        ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
        ANDROID_CONTROL_AF_MODE,
        ANDROID_CONTROL_AF_STATE,
        ANDROID_CONTROL_AF_TRIGGER,
        ANDROID_CONTROL_AWB_LOCK,
        ANDROID_CONTROL_AWB_MODE,
        ANDROID_CONTROL_AWB_STATE,
        ANDROID_CONTROL_CAPTURE_INTENT,
        ANDROID_CONTROL_EFFECT_MODE,
        ANDROID_CONTROL_MODE,
        ANDROID_CONTROL_SCENE_MODE,
        ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
        ANDROID_FLASH_MODE,
        ANDROID_FLASH_STATE,
        ANDROID_JPEG_ORIENTATION,
        ANDROID_JPEG_QUALITY,
        ANDROID_JPEG_THUMBNAIL_QUALITY,
        ANDROID_JPEG_THUMBNAIL_SIZE,
        ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
        ANDROID_NOISE_REDUCTION_MODE,
        ANDROID_REQUEST_PIPELINE_DEPTH,
        ANDROID_SCALER_CROP_REGION,
        ANDROID_SENSOR_TIMESTAMP,
        ANDROID_STATISTICS_FACE_DETECT_MODE,
        ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE,
        ANDROID_STATISTICS_LENS_SHADING_MAP_MODE,
        ANDROID_STATISTICS_SCENE_FLICKER};
    UPDATE(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, availableResultKeys,
           ARRAY_SIZE(availableResultKeys));

    const int32_t availableCharacteristicsKeys[] = {
        ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
        ANDROID_CONTROL_AE_COMPENSATION_RANGE,
        ANDROID_CONTROL_AE_COMPENSATION_STEP,
        ANDROID_CONTROL_AE_LOCK_AVAILABLE,
        ANDROID_CONTROL_AF_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_EFFECTS,
        ANDROID_CONTROL_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
        ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
        ANDROID_CONTROL_AWB_AVAILABLE_MODES,
        ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
        ANDROID_CONTROL_MAX_REGIONS,
        ANDROID_FLASH_INFO_AVAILABLE,
        ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
        ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
        ANDROID_LENS_FACING,
        ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
        ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
        ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
        ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
        ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
        ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
        ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
        ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        ANDROID_SCALER_CROPPING_TYPE,
        ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
        ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
        ANDROID_SENSOR_ORIENTATION,
        ANDROID_SHADING_AVAILABLE_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
        ANDROID_STATISTICS_INFO_MAX_FACE_COUNT,
        ANDROID_SYNC_MAX_LATENCY};
    UPDATE(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
           availableCharacteristicsKeys,
           ARRAY_SIZE(availableCharacteristicsKeys));

    return OK;
}

status_t ExternalCameraDevice::initCameraControlsCharsKeys(int,
        ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata) {
    /**
     * android.sensor.info.sensitivityRange   -> V4L2_CID_ISO_SENSITIVITY
     * android.sensor.info.exposureTimeRange  -> V4L2_CID_EXPOSURE_ABSOLUTE
     * android.sensor.info.maxFrameDuration   -> TBD
     * android.lens.info.minimumFocusDistance -> V4L2_CID_FOCUS_ABSOLUTE
     * android.lens.info.hyperfocalDistance
     * android.lens.info.availableFocalLengths -> not available?
     */

    // android.control
    // No AE compensation support for now.
    // TODO: V4L2_CID_EXPOSURE_BIAS
    const int32_t controlAeCompensationRange[] = {0, 0};
    UPDATE(ANDROID_CONTROL_AE_COMPENSATION_RANGE, controlAeCompensationRange,
           ARRAY_SIZE(controlAeCompensationRange));
    const camera_metadata_rational_t controlAeCompensationStep[] = {{0, 1}};
    UPDATE(ANDROID_CONTROL_AE_COMPENSATION_STEP, controlAeCompensationStep,
           ARRAY_SIZE(controlAeCompensationStep));


    // TODO: Check V4L2_CID_AUTO_FOCUS_*.
    const uint8_t afAvailableModes[] = {ANDROID_CONTROL_AF_MODE_AUTO,
                                        ANDROID_CONTROL_AF_MODE_OFF};
    UPDATE(ANDROID_CONTROL_AF_AVAILABLE_MODES, afAvailableModes,
           ARRAY_SIZE(afAvailableModes));

    // TODO: V4L2_CID_SCENE_MODE
    const uint8_t availableSceneMode = ANDROID_CONTROL_SCENE_MODE_DISABLED;
    UPDATE(ANDROID_CONTROL_AVAILABLE_SCENE_MODES, &availableSceneMode, 1);

    // TODO: V4L2_CID_3A_LOCK
    const uint8_t aeLockAvailable = ANDROID_CONTROL_AE_LOCK_AVAILABLE_FALSE;
    UPDATE(ANDROID_CONTROL_AE_LOCK_AVAILABLE, &aeLockAvailable, 1);
    const uint8_t awbLockAvailable = ANDROID_CONTROL_AWB_LOCK_AVAILABLE_FALSE;
    UPDATE(ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &awbLockAvailable, 1);

    // TODO: V4L2_CID_ZOOM_*
    const float scalerAvailableMaxDigitalZoom[] = {1};
    UPDATE(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
           scalerAvailableMaxDigitalZoom,
           ARRAY_SIZE(scalerAvailableMaxDigitalZoom));

    return OK;
}

status_t ExternalCameraDevice::initOutputCharsKeys(int fd,
        ::android::hardware::camera::common::V1_0::helper::CameraMetadata* metadata) {
    initSupportedFormatsLocked(fd);
    if (mSupportedFormats.empty()) {
        ALOGE("%s: Init supported format list failed", __FUNCTION__);
        return UNKNOWN_ERROR;
    }

    std::vector<int32_t> streamConfigurations;
    std::vector<int64_t> minFrameDurations;
    std::vector<int64_t> stallDurations;
    int64_t maxFrameDuration = 0;
    int32_t maxFps = std::numeric_limits<int32_t>::min();
    int32_t minFps = std::numeric_limits<int32_t>::max();
    std::set<int32_t> framerates;

    std::array<int, /*size*/3> halFormats{{
        HAL_PIXEL_FORMAT_BLOB,
        HAL_PIXEL_FORMAT_YCbCr_420_888,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED}};

    for (const auto& supportedFormat : mSupportedFormats) {
        for (const auto& format : halFormats) {
            streamConfigurations.push_back(format);
            streamConfigurations.push_back(supportedFormat.width);
            streamConfigurations.push_back(supportedFormat.height);
            streamConfigurations.push_back(
                    ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
        }

        int64_t min_frame_duration = std::numeric_limits<int64_t>::max();
        for (const auto& frameRate : supportedFormat.frameRates) {
            int64_t frame_duration = 1000000000LL / frameRate;
            if (frame_duration < min_frame_duration) {
                min_frame_duration = frame_duration;
            }
            if (frame_duration > maxFrameDuration) {
                maxFrameDuration = frame_duration;
            }
            int32_t frameRateInt = static_cast<int32_t>(frameRate);
            if (minFps > frameRateInt) {
                minFps = frameRateInt;
            }
            if (maxFps < frameRateInt) {
                maxFps = frameRateInt;
            }
            framerates.insert(frameRateInt);
        }

        for (const auto& format : halFormats) {
            minFrameDurations.push_back(format);
            minFrameDurations.push_back(supportedFormat.width);
            minFrameDurations.push_back(supportedFormat.height);
            minFrameDurations.push_back(min_frame_duration);
        }

        // The stall duration is 0 for non-jpeg formats. For JPEG format, stall
        // duration can be 0 if JPEG is small. Here we choose 1 sec for JPEG.
        // TODO: b/72261675. Maybe set this dynamically
        for (const auto& format : halFormats) {
            const int64_t NS_TO_SECOND = 1000000000;
            int64_t stall_duration =
                    (format == HAL_PIXEL_FORMAT_BLOB) ? NS_TO_SECOND : 0;
            stallDurations.push_back(format);
            stallDurations.push_back(supportedFormat.width);
            stallDurations.push_back(supportedFormat.height);
            stallDurations.push_back(stall_duration);
        }
    }

    // The document in aeAvailableTargetFpsRanges section says the minFps should
    // not be larger than 15.
    // We cannot support fixed 30fps but Android requires (min, max) and
    // (max, max) ranges.
    // TODO: populate more, right now this does not support 30,30 if the device
    //       has higher than 30 fps modes
    std::vector<int32_t> fpsRanges;
    // Variable range
    fpsRanges.push_back(minFps);
    fpsRanges.push_back(maxFps);
    // Fixed ranges
    for (const auto& framerate : framerates) {
        fpsRanges.push_back(framerate);
        fpsRanges.push_back(framerate);
    }
    UPDATE(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fpsRanges.data(),
           fpsRanges.size());

    UPDATE(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
           streamConfigurations.data(), streamConfigurations.size());

    UPDATE(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
           minFrameDurations.data(), minFrameDurations.size());

    UPDATE(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stallDurations.data(),
           stallDurations.size());

    UPDATE(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, &maxFrameDuration, 1);

    SupportedV4L2Format maximumFormat {.width = 0, .height = 0};
    for (const auto& supportedFormat : mSupportedFormats) {
        if (supportedFormat.width >= maximumFormat.width &&
            supportedFormat.height >= maximumFormat.height) {
            maximumFormat = supportedFormat;
        }
    }
    int32_t activeArraySize[] = {0, 0,
                                 static_cast<int32_t>(maximumFormat.width),
                                 static_cast<int32_t>(maximumFormat.height)};
    UPDATE(ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
           activeArraySize, ARRAY_SIZE(activeArraySize));
    UPDATE(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArraySize,
           ARRAY_SIZE(activeArraySize));

    int32_t pixelArraySize[] = {static_cast<int32_t>(maximumFormat.width),
                                static_cast<int32_t>(maximumFormat.height)};
    UPDATE(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, pixelArraySize,
           ARRAY_SIZE(pixelArraySize));
    return OK;
}

#undef ARRAY_SIZE
#undef UPDATE

void ExternalCameraDevice::getFrameRateList(
        int fd, SupportedV4L2Format* format) {
    format->frameRates.clear();

    v4l2_frmivalenum frameInterval {
        .pixel_format = format->fourcc,
        .width = format->width,
        .height = format->height,
        .index = 0
    };

    for (frameInterval.index = 0;
            TEMP_FAILURE_RETRY(ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frameInterval)) == 0;
            ++frameInterval.index) {
        if (frameInterval.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            if (frameInterval.discrete.numerator != 0) {
                float framerate = frameInterval.discrete.denominator /
                        static_cast<float>(frameInterval.discrete.numerator);
                ALOGV("index:%d, format:%c%c%c%c, w %d, h %d, framerate %f",
                    frameInterval.index,
                    frameInterval.pixel_format & 0xFF,
                    (frameInterval.pixel_format >> 8) & 0xFF,
                    (frameInterval.pixel_format >> 16) & 0xFF,
                    (frameInterval.pixel_format >> 24) & 0xFF,
                    frameInterval.width, frameInterval.height, framerate);
                format->frameRates.push_back(framerate);
            }
        }
    }

    if (format->frameRates.empty()) {
        ALOGE("%s: failed to get supported frame rates for format:%c%c%c%c w %d h %d",
                __FUNCTION__,
                frameInterval.pixel_format & 0xFF,
                (frameInterval.pixel_format >> 8) & 0xFF,
                (frameInterval.pixel_format >> 16) & 0xFF,
                (frameInterval.pixel_format >> 24) & 0xFF,
                frameInterval.width, frameInterval.height);
    }
}

void ExternalCameraDevice::initSupportedFormatsLocked(int fd) {
    struct v4l2_fmtdesc fmtdesc {
        .index = 0,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    int ret = 0;
    while (ret == 0) {
        ret = TEMP_FAILURE_RETRY(ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc));
        ALOGD("index:%d,ret:%d, format:%c%c%c%c", fmtdesc.index, ret,
                fmtdesc.pixelformat & 0xFF,
                (fmtdesc.pixelformat >> 8) & 0xFF,
                (fmtdesc.pixelformat >> 16) & 0xFF,
                (fmtdesc.pixelformat >> 24) & 0xFF);
        if (ret == 0 && !(fmtdesc.flags & V4L2_FMT_FLAG_EMULATED)) {
            auto it = std::find (
                    kSupportedFourCCs.begin(), kSupportedFourCCs.end(), fmtdesc.pixelformat);
            if (it != kSupportedFourCCs.end()) {
                // Found supported format
                v4l2_frmsizeenum frameSize {
                        .index = 0,
                        .pixel_format = fmtdesc.pixelformat};
                for (; TEMP_FAILURE_RETRY(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frameSize)) == 0;
                        ++frameSize.index) {
                    if (frameSize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                        ALOGD("index:%d, format:%c%c%c%c, w %d, h %d", frameSize.index,
                            fmtdesc.pixelformat & 0xFF,
                            (fmtdesc.pixelformat >> 8) & 0xFF,
                            (fmtdesc.pixelformat >> 16) & 0xFF,
                            (fmtdesc.pixelformat >> 24) & 0xFF,
                            frameSize.discrete.width, frameSize.discrete.height);
                        // Disregard h > w formats so all aspect ratio (h/w) <= 1.0
                        // This will simplify the crop/scaling logic down the road
                        if (frameSize.discrete.height > frameSize.discrete.width) {
                            continue;
                        }
                        SupportedV4L2Format format {
                            .width = frameSize.discrete.width,
                            .height = frameSize.discrete.height,
                            .fourcc = fmtdesc.pixelformat
                        };
                        getFrameRateList(fd, &format);
                        if (!format.frameRates.empty()) {
                            mSupportedFormats.push_back(format);
                        }
                    }
                }
            }
        }
        fmtdesc.index++;
    }
}

}  // namespace implementation
}  // namespace V3_4
}  // namespace device
}  // namespace camera
}  // namespace hardware
}  // namespace android

