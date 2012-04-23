/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "Camera2_test"
#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <gtest/gtest.h>
#include <iostream>
#include <fstream>

#include <utils/Vector.h>
#include <gui/CpuConsumer.h>
#include <system/camera_metadata.h>

#include "camera2_utils.h"

namespace android {

class Camera2Test: public testing::Test {
  public:
    static void SetUpTestCase() {
        int res;

        hw_module_t *module = NULL;
        res = hw_get_module(CAMERA_HARDWARE_MODULE_ID,
                (const hw_module_t **)&module);

        ASSERT_EQ(0, res)
                << "Failure opening camera hardware module: " << res;
        ASSERT_TRUE(NULL != module)
                << "No camera module was set by hw_get_module";

        IF_ALOGV() {
            std::cout << "  Camera module name: "
                    << module->name << std::endl;
            std::cout << "  Camera module author: "
                    << module->author << std::endl;
            std::cout << "  Camera module API version: 0x" << std::hex
                    << module->module_api_version << std::endl;
            std::cout << "  Camera module HAL API version: 0x" << std::hex
                    << module->hal_api_version << std::endl;
        }

        int16_t version2_0 = CAMERA_MODULE_API_VERSION_2_0;
        ASSERT_EQ(version2_0, module->module_api_version)
                << "Camera module version is 0x"
                << std::hex << module->module_api_version
                << ", not 2.0. (0x"
                << std::hex << CAMERA_MODULE_API_VERSION_2_0 << ")";

        sCameraModule = reinterpret_cast<camera_module_t*>(module);

        sNumCameras = sCameraModule->get_number_of_cameras();
        ASSERT_LT(0, sNumCameras) << "No camera devices available!";

        IF_ALOGV() {
            std::cout << "  Camera device count: " << sNumCameras << std::endl;
        }

        sCameraSupportsHal2 = new bool[sNumCameras];

        for (int i = 0; i < sNumCameras; i++) {
            camera_info info;
            res = sCameraModule->get_camera_info(i, &info);
            ASSERT_EQ(0, res)
                    << "Failure getting camera info for camera " << i;
            IF_ALOGV() {
                std::cout << "  Camera device: " << std::dec
                          << i << std::endl;;
                std::cout << "    Facing: " << std::dec
                          << info.facing  << std::endl;
                std::cout << "    Orientation: " << std::dec
                          << info.orientation  << std::endl;
                std::cout << "    Version: 0x" << std::hex <<
                        info.device_version  << std::endl;
            }
            if (info.device_version >= CAMERA_DEVICE_API_VERSION_2_0) {
                sCameraSupportsHal2[i] = true;
                ASSERT_TRUE(NULL != info.static_camera_characteristics);
                IF_ALOGV() {
                    std::cout << "    Static camera metadata:"  << std::endl;
                    dump_camera_metadata(info.static_camera_characteristics,
                            0, 1);
                }
            } else {
                sCameraSupportsHal2[i] = false;
            }
        }
    }

    static const camera_module_t *getCameraModule() {
        return sCameraModule;
    }

    static int getNumCameras() {
        return sNumCameras;
    }

    static bool isHal2Supported(int id) {
        return sCameraSupportsHal2[id];
    }

    static camera2_device_t *openCameraDevice(int id) {
        ALOGV("Opening camera %d", id);
        if (NULL == sCameraSupportsHal2) return NULL;
        if (id >= sNumCameras) return NULL;
        if (!sCameraSupportsHal2[id]) return NULL;

        hw_device_t *device = NULL;
        const camera_module_t *cam_module = getCameraModule();
        if (cam_module == NULL) {
            return NULL;
        }

        char camId[10];
        int res;

        snprintf(camId, 10, "%d", id);
        res = cam_module->common.methods->open(
            (const hw_module_t*)cam_module,
            camId,
            &device);
        if (res != NO_ERROR || device == NULL) {
            return NULL;
        }
        camera2_device_t *cam_device =
                reinterpret_cast<camera2_device_t*>(device);
        return cam_device;
    }

    static status_t configureCameraDevice(camera2_device_t *dev,
            MetadataQueue &requestQueue,
            MetadataQueue  &frameQueue,
            NotifierListener &listener) {

        status_t err;

        err = dev->ops->set_request_queue_src_ops(dev,
                requestQueue.getToConsumerInterface());
        if (err != OK) return err;

        requestQueue.setFromConsumerInterface(dev);

        err = dev->ops->set_frame_queue_dst_ops(dev,
                frameQueue.getToProducerInterface());
        if (err != OK) return err;

        err = listener.getNotificationsFrom(dev);
        if (err != OK) return err;

        vendor_tag_query_ops_t *vendor_metadata_tag_ops;
        err = dev->ops->get_metadata_vendor_tag_ops(dev, &vendor_metadata_tag_ops);
        if (err != OK) return err;

        err = set_camera_metadata_vendor_tag_ops(vendor_metadata_tag_ops);
        if (err != OK) return err;

        return OK;
    }

    static status_t closeCameraDevice(camera2_device_t *cam_dev) {
        int res;
        ALOGV("Closing camera %p", cam_dev);

        hw_device_t *dev = reinterpret_cast<hw_device_t *>(cam_dev);
        res = dev->close(dev);
        return res;
    }

    void setUpCamera(int id) {
        ASSERT_GT(sNumCameras, id);
        status_t res;

        if (mDevice != NULL) {
            closeCameraDevice(mDevice);
        }
        mDevice = openCameraDevice(id);
        ASSERT_TRUE(NULL != mDevice) << "Failed to open camera device";

        camera_info info;
        res = sCameraModule->get_camera_info(id, &info);
        ASSERT_EQ(OK, res);

        mStaticInfo = info.static_camera_characteristics;

        res = configureCameraDevice(mDevice,
                mRequests,
                mFrames,
                mNotifications);
        ASSERT_EQ(OK, res) << "Failure to configure camera device";

    }

    void setUpStream(sp<ISurfaceTexture> consumer,
            int width, int height, int format, int *id) {
        status_t res;

        StreamAdapter* stream = new StreamAdapter(consumer);

        ALOGV("Creating stream, format 0x%x, %d x %d", format, width, height);
        res = stream->connectToDevice(mDevice, width, height, format);
        ASSERT_EQ(NO_ERROR, res) << "Failed to connect to stream: "
                                 << strerror(-res);
        mStreams.push_back(stream);

        *id = stream->getId();
    }

    void disconnectStream(int id) {
        status_t res;
        unsigned int i=0;
        for (; i < mStreams.size(); i++) {
            if (mStreams[i]->getId() == id) {
                res = mStreams[i]->disconnect();
                ASSERT_EQ(NO_ERROR, res) <<
                        "Failed to disconnect stream " << id;
                break;
            }
        }
        ASSERT_GT(mStreams.size(), i) << "Stream id not found:" << id;
    }

    void getResolutionList(uint32_t format,
            uint32_t **list,
            size_t *count) {

        uint32_t *availableFormats;
        size_t   availableFormatsCount;
        status_t res;
        res = find_camera_metadata_entry(mStaticInfo,
                ANDROID_SCALER_AVAILABLE_FORMATS,
                NULL,
                (void**)&availableFormats,
                &availableFormatsCount);
        ASSERT_EQ(OK, res);

        uint32_t formatIdx;
        for (formatIdx=0; formatIdx < availableFormatsCount; formatIdx++) {
            if (availableFormats[formatIdx] == format) break;
        }
        ASSERT_NE(availableFormatsCount, formatIdx)
                << "No support found for format 0x" << std::hex << format;

        uint32_t *availableSizesPerFormat;
        size_t    availableSizesPerFormatCount;
        res = find_camera_metadata_entry(mStaticInfo,
                ANDROID_SCALER_AVAILABLE_SIZES_PER_FORMAT,
                NULL,
                (void**)&availableSizesPerFormat,
                &availableSizesPerFormatCount);
        ASSERT_EQ(OK, res);

        int size_offset = 0;
        for (unsigned int i=0; i < formatIdx; i++) {
            size_offset += availableSizesPerFormat[i];
        }

        uint32_t *availableSizes;
        size_t    availableSizesCount;
        res = find_camera_metadata_entry(mStaticInfo,
                ANDROID_SCALER_AVAILABLE_SIZES,
                NULL,
                (void**)&availableSizes,
                &availableSizesCount);
        ASSERT_EQ(OK, res);

        *list = availableSizes + size_offset;
        *count = availableSizesPerFormat[formatIdx];
    }

    virtual void SetUp() {
        const ::testing::TestInfo* const testInfo =
                ::testing::UnitTest::GetInstance()->current_test_info();

        ALOGV("*** Starting test %s in test case %s", testInfo->name(), testInfo->test_case_name());
        mDevice = NULL;
    }

    virtual void TearDown() {
        for (unsigned int i = 0; i < mStreams.size(); i++) {
            delete mStreams[i];
        }
        if (mDevice != NULL) {
            closeCameraDevice(mDevice);
        }
    }

    camera2_device    *mDevice;
    camera_metadata_t *mStaticInfo;

    MetadataQueue    mRequests;
    MetadataQueue    mFrames;
    NotifierListener mNotifications;

    Vector<StreamAdapter*> mStreams;

  private:
    static camera_module_t *sCameraModule;
    static int              sNumCameras;
    static bool            *sCameraSupportsHal2;
};

camera_module_t *Camera2Test::sCameraModule = NULL;
bool *Camera2Test::sCameraSupportsHal2      = NULL;
int Camera2Test::sNumCameras                = 0;

static const nsecs_t USEC = 1000;
static const nsecs_t MSEC = 1000*USEC;
static const nsecs_t SEC = 1000*MSEC;


TEST_F(Camera2Test, OpenClose) {
    status_t res;

    for (int id = 0; id < getNumCameras(); id++) {
        if (!isHal2Supported(id)) continue;

        camera2_device_t *d = openCameraDevice(id);
        ASSERT_TRUE(NULL != d) << "Failed to open camera device";

        res = closeCameraDevice(d);
        ASSERT_EQ(NO_ERROR, res) << "Failed to close camera device";
    }
}

TEST_F(Camera2Test, Capture1Raw) {
    status_t res;

    for (int id = 0; id < getNumCameras(); id++) {
        if (!isHal2Supported(id)) continue;

        ASSERT_NO_FATAL_FAILURE(setUpCamera(id));

        sp<CpuConsumer> rawConsumer = new CpuConsumer(1);
        sp<FrameWaiter> rawWaiter = new FrameWaiter();
        rawConsumer->setFrameAvailableListener(rawWaiter);

        uint32_t *rawResolutions;
        size_t    rawResolutionsCount;

        int format = HAL_PIXEL_FORMAT_RAW_SENSOR;

        getResolutionList(format,
                &rawResolutions, &rawResolutionsCount);
        ASSERT_LT((uint32_t)0, rawResolutionsCount);

        // Pick first available raw resolution
        int width = rawResolutions[0];
        int height = rawResolutions[1];

        int streamId;
        ASSERT_NO_FATAL_FAILURE(
            setUpStream(rawConsumer->getProducerInterface(),
                    width, height, format, &streamId) );

        camera_metadata_t *request;
        request = allocate_camera_metadata(20, 2000);

        uint8_t metadataMode = ANDROID_REQUEST_METADATA_FULL;
        add_camera_metadata_entry(request,
                ANDROID_REQUEST_METADATA_MODE,
                (void**)&metadataMode, 1);
        uint32_t outputStreams = streamId;
        add_camera_metadata_entry(request,
                ANDROID_REQUEST_OUTPUT_STREAMS,
                (void**)&outputStreams, 1);

        uint64_t exposureTime = 2*MSEC;
        add_camera_metadata_entry(request,
                ANDROID_SENSOR_EXPOSURE_TIME,
                (void**)&exposureTime, 1);
        uint64_t frameDuration = 30*MSEC;
        add_camera_metadata_entry(request,
                ANDROID_SENSOR_FRAME_DURATION,
                (void**)&frameDuration, 1);
        uint32_t sensitivity = 100;
        add_camera_metadata_entry(request,
                ANDROID_SENSOR_SENSITIVITY,
                (void**)&sensitivity, 1);

        uint32_t hourOfDay = 12;
        add_camera_metadata_entry(request,
                0x80000000, // EMULATOR_HOUROFDAY
                &hourOfDay, 1);

        IF_ALOGV() {
            std::cout << "Input request: " << std::endl;
            dump_camera_metadata(request, 0, 1);
        }

        res = mRequests.enqueue(request);
        ASSERT_EQ(NO_ERROR, res) << "Can't enqueue request: " << strerror(-res);

        res = mFrames.waitForBuffer(exposureTime + SEC);
        ASSERT_EQ(NO_ERROR, res) << "No frame to get: " << strerror(-res);

        camera_metadata_t *frame;
        res = mFrames.dequeue(&frame);
        ASSERT_EQ(NO_ERROR, res);
        ASSERT_TRUE(frame != NULL);

        IF_ALOGV() {
            std::cout << "Output frame:" << std::endl;
            dump_camera_metadata(frame, 0, 1);
        }

        res = rawWaiter->waitForFrame(exposureTime + SEC);
        ASSERT_EQ(NO_ERROR, res);

        CpuConsumer::LockedBuffer buffer;
        res = rawConsumer->lockNextBuffer(&buffer);
        ASSERT_EQ(NO_ERROR, res);

        IF_ALOGV() {
            const char *dumpname =
                    "/data/local/tmp/camera2_test-capture1raw-dump.raw";
            ALOGV("Dumping raw buffer to %s", dumpname);
            // Write to file
            std::ofstream rawFile(dumpname);
            for (unsigned int y = 0; y < buffer.height; y++) {
                rawFile.write((const char *)(buffer.data + y * buffer.stride * 2),
                        buffer.width * 2);
            }
            rawFile.close();
        }

        res = rawConsumer->unlockBuffer(buffer);
        ASSERT_EQ(NO_ERROR, res);

        ASSERT_NO_FATAL_FAILURE(disconnectStream(streamId));

        res = closeCameraDevice(mDevice);
        ASSERT_EQ(NO_ERROR, res) << "Failed to close camera device";

    }
}

TEST_F(Camera2Test, CaptureBurstRaw) {
    status_t res;

    for (int id = 0; id < getNumCameras(); id++) {
        if (!isHal2Supported(id)) continue;

        ASSERT_NO_FATAL_FAILURE(setUpCamera(id));

        sp<CpuConsumer> rawConsumer = new CpuConsumer(1);
        sp<FrameWaiter> rawWaiter = new FrameWaiter();
        rawConsumer->setFrameAvailableListener(rawWaiter);

        uint32_t *rawResolutions;
        size_t    rawResolutionsCount;

        int format = HAL_PIXEL_FORMAT_RAW_SENSOR;

        getResolutionList(format,
                &rawResolutions, &rawResolutionsCount);
        ASSERT_LT((uint32_t)0, rawResolutionsCount);

        // Pick first available raw resolution
        int width = rawResolutions[0];
        int height = rawResolutions[1];

        int streamId;
        ASSERT_NO_FATAL_FAILURE(
            setUpStream(rawConsumer->getProducerInterface(),
                    width, height, format, &streamId) );

        camera_metadata_t *request;
        request = allocate_camera_metadata(20, 2000);

        uint8_t metadataMode = ANDROID_REQUEST_METADATA_FULL;
        add_camera_metadata_entry(request,
                ANDROID_REQUEST_METADATA_MODE,
                (void**)&metadataMode, 1);
        uint32_t outputStreams = streamId;
        add_camera_metadata_entry(request,
                ANDROID_REQUEST_OUTPUT_STREAMS,
                (void**)&outputStreams, 1);

        uint64_t frameDuration = 30*MSEC;
        add_camera_metadata_entry(request,
                ANDROID_SENSOR_FRAME_DURATION,
                (void**)&frameDuration, 1);
        uint32_t sensitivity = 100;
        add_camera_metadata_entry(request,
                ANDROID_SENSOR_SENSITIVITY,
                (void**)&sensitivity, 1);

        uint32_t hourOfDay = 12;
        add_camera_metadata_entry(request,
                0x80000000, // EMULATOR_HOUROFDAY
                &hourOfDay, 1);

        IF_ALOGV() {
            std::cout << "Input request template: " << std::endl;
            dump_camera_metadata(request, 0, 1);
        }

        int numCaptures = 10;

        // Enqueue numCaptures requests with increasing exposure time

        uint64_t exposureTime = 1 * MSEC;
        for (int reqCount = 0; reqCount < numCaptures; reqCount++ ) {
            camera_metadata_t *req;
            req = allocate_camera_metadata(20, 2000);
            append_camera_metadata(req, request);

            add_camera_metadata_entry(req,
                    ANDROID_SENSOR_EXPOSURE_TIME,
                    (void**)&exposureTime, 1);
            exposureTime *= 2;

            res = mRequests.enqueue(req);
            ASSERT_EQ(NO_ERROR, res) << "Can't enqueue request: "
                    << strerror(-res);
        }

        // Get frames and image buffers one by one
        for (int frameCount = 0; frameCount < 10; frameCount++) {
            res = mFrames.waitForBuffer(SEC);
            ASSERT_EQ(NO_ERROR, res) << "No frame to get: " << strerror(-res);

            camera_metadata_t *frame;
            res = mFrames.dequeue(&frame);
            ASSERT_EQ(NO_ERROR, res);
            ASSERT_TRUE(frame != NULL);

            uint32_t *frameNumber;
            res = find_camera_metadata_entry(frame,
                    ANDROID_REQUEST_FRAME_COUNT,
                    NULL, (void**)&frameNumber, NULL);
            ASSERT_EQ(NO_ERROR, res);
            ASSERT_EQ(frameCount, *frameNumber);

            res = rawWaiter->waitForFrame(SEC);
            ASSERT_EQ(NO_ERROR, res) <<
                    "Never got raw data for capture " << frameCount;

            CpuConsumer::LockedBuffer buffer;
            res = rawConsumer->lockNextBuffer(&buffer);
            ASSERT_EQ(NO_ERROR, res);

            IF_ALOGV() {
                char dumpname[60];
                snprintf(dumpname, 60,
                        "/data/local/tmp/camera2_test-capture1raw-dump_%d.raw",
                        frameCount);
                ALOGV("Dumping raw buffer to %s", dumpname);
                // Write to file
                std::ofstream rawFile(dumpname);
                for (unsigned int y = 0; y < buffer.height; y++) {
                    rawFile.write(
                            (const char *)(buffer.data + y * buffer.stride * 2),
                            buffer.width * 2);
                }
                rawFile.close();
            }

            res = rawConsumer->unlockBuffer(buffer);
            ASSERT_EQ(NO_ERROR, res);
        }
    }
}

} // namespace android
