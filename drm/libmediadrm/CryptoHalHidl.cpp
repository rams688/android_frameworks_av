/*
 * Copyright (C) 2017 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "CryptoHalHidl"
#include <utils/Log.h>

#include <android/hardware/drm/1.0/types.h>
#include <android/hidl/manager/1.2/IServiceManager.h>
#include <hidl/ServiceManagement.h>
#include <hidlmemory/FrameworkUtils.h>
#include <media/hardware/CryptoAPI.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/foundation/hexdump.h>
#include <mediadrm/CryptoHalHidl.h>
#include <mediadrm/DrmUtils.h>

using drm::V1_0::BufferType;
using drm::V1_0::DestinationBuffer;
using drm::V1_0::ICryptoFactory;
using drm::V1_0::ICryptoPlugin;
using drm::V1_0::Mode;
using drm::V1_0::Pattern;
using drm::V1_0::SharedBuffer;
using drm::V1_0::Status;
using drm::V1_0::SubSample;

using ::android::sp;
using ::android::DrmUtils::toStatusT;
using ::android::hardware::hidl_array;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_memory;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::HidlMemory;
using ::android::hardware::Return;
using ::android::hardware::Void;

typedef drm::V1_2::Status Status_V1_2;

namespace android {

static hidl_vec<uint8_t> toHidlVec(const Vector<uint8_t>& vector) {
    hidl_vec<uint8_t> vec;
    vec.setToExternal(const_cast<uint8_t*>(vector.array()), vector.size());
    return vec;
}

static hidl_vec<uint8_t> toHidlVec(const void* ptr, size_t size) {
    hidl_vec<uint8_t> vec;
    vec.resize(size);
    memcpy(vec.data(), ptr, size);
    return vec;
}

static hidl_array<uint8_t, 16> toHidlArray16(const uint8_t* ptr) {
    if (!ptr) {
        return hidl_array<uint8_t, 16>();
    }
    return hidl_array<uint8_t, 16>(ptr);
}

static String8 toString8(hidl_string hString) {
    return String8(hString.c_str());
}

CryptoHalHidl::CryptoHalHidl()
    : mFactories(makeCryptoFactories()),
      mInitCheck((mFactories.size() == 0) ? ERROR_UNSUPPORTED : NO_INIT),
      mHeapSeqNum(0) {}

CryptoHalHidl::~CryptoHalHidl() {}

Vector<sp<ICryptoFactory>> CryptoHalHidl::makeCryptoFactories() {
    Vector<sp<ICryptoFactory>> factories;

    auto manager = hardware::defaultServiceManager1_2();
    if (manager != NULL) {
        manager->listManifestByInterface(
                drm::V1_0::ICryptoFactory::descriptor,
                [&factories](const hidl_vec<hidl_string>& registered) {
                    for (const auto& instance : registered) {
                        auto factory = drm::V1_0::ICryptoFactory::getService(instance);
                        if (factory != NULL) {
                            ALOGD("found drm@1.0 ICryptoFactory %s", instance.c_str());
                            factories.push_back(factory);
                        }
                    }
                });
        manager->listManifestByInterface(
                drm::V1_1::ICryptoFactory::descriptor,
                [&factories](const hidl_vec<hidl_string>& registered) {
                    for (const auto& instance : registered) {
                        auto factory = drm::V1_1::ICryptoFactory::getService(instance);
                        if (factory != NULL) {
                            ALOGD("found drm@1.1 ICryptoFactory %s", instance.c_str());
                            factories.push_back(factory);
                        }
                    }
                });
    }

    if (factories.size() == 0) {
        // must be in passthrough mode, load the default passthrough service
        auto passthrough = ICryptoFactory::getService();
        if (passthrough != NULL) {
            ALOGI("makeCryptoFactories: using default passthrough crypto instance");
            factories.push_back(passthrough);
        } else {
            ALOGE("Failed to find any crypto factories");
        }
    }
    return factories;
}

sp<ICryptoPlugin> CryptoHalHidl::makeCryptoPlugin(const sp<ICryptoFactory>& factory,
                                                  const uint8_t uuid[16], const void* initData,
                                                  size_t initDataSize) {
    sp<ICryptoPlugin> plugin;
    Return<void> hResult =
            factory->createPlugin(toHidlArray16(uuid), toHidlVec(initData, initDataSize),
                                  [&](Status status, const sp<ICryptoPlugin>& hPlugin) {
                                      if (status != Status::OK) {
                                          ALOGE("Failed to make crypto plugin");
                                          return;
                                      }
                                      plugin = hPlugin;
                                  });
    if (!hResult.isOk()) {
        mInitCheck = DEAD_OBJECT;
    }
    return plugin;
}

status_t CryptoHalHidl::initCheck() const {
    return mInitCheck;
}

bool CryptoHalHidl::isCryptoSchemeSupported(const uint8_t uuid[16]) {
    Mutex::Autolock autoLock(mLock);

    for (size_t i = 0; i < mFactories.size(); i++) {
        if (mFactories[i]->isCryptoSchemeSupported(uuid)) {
            return true;
        }
    }
    return false;
}

status_t CryptoHalHidl::createPlugin(const uint8_t uuid[16], const void* data, size_t size) {
    Mutex::Autolock autoLock(mLock);

    for (size_t i = 0; i < mFactories.size(); i++) {
        if (mFactories[i]->isCryptoSchemeSupported(uuid)) {
            mPlugin = makeCryptoPlugin(mFactories[i], uuid, data, size);
            if (mPlugin != NULL) {
                mPluginV1_2 = drm::V1_2::ICryptoPlugin::castFrom(mPlugin);
            }
        }
    }

    if (mInitCheck == NO_INIT) {
        mInitCheck = mPlugin == NULL ? ERROR_UNSUPPORTED : OK;
    }

    return mInitCheck;
}

status_t CryptoHalHidl::destroyPlugin() {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    mPlugin.clear();
    mPluginV1_2.clear();
    mInitCheck = NO_INIT;
    return OK;
}

bool CryptoHalHidl::requiresSecureDecoderComponent(const char* mime) const {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return false;
    }

    Return<bool> hResult = mPlugin->requiresSecureDecoderComponent(hidl_string(mime));
    if (!hResult.isOk()) {
        return false;
    }
    return hResult;
}

/**
 * If the heap base isn't set, get the heap base from the HidlMemory
 * and send it to the HAL so it can map a remote heap of the same
 * size.  Once the heap base is established, shared memory buffers
 * are sent by providing an offset into the heap and a buffer size.
 */
int32_t CryptoHalHidl::setHeapBase(const sp<HidlMemory>& heap) {
    if (heap == NULL || mHeapSeqNum < 0) {
        ALOGE("setHeapBase(): heap %p mHeapSeqNum %d", heap.get(), mHeapSeqNum);
        return -1;
    }

    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return -1;
    }

    int32_t seqNum = mHeapSeqNum++;
    uint32_t bufferId = static_cast<uint32_t>(seqNum);
    mHeapSizes.add(seqNum, heap->size());
    Return<void> hResult = mPlugin->setSharedBufferBase(*heap, bufferId);
    ALOGE_IF(!hResult.isOk(), "setSharedBufferBase(): remote call failed");
    return seqNum;
}

void CryptoHalHidl::clearHeapBase(int32_t seqNum) {
    Mutex::Autolock autoLock(mLock);

    /*
     * Clear the remote shared memory mapping by setting the shared
     * buffer base to a null hidl_memory.
     *
     * TODO: Add a releaseSharedBuffer method in a future DRM HAL
     * API version to make this explicit.
     */
    ssize_t index = mHeapSizes.indexOfKey(seqNum);
    if (index >= 0) {
        if (mPlugin != NULL) {
            uint32_t bufferId = static_cast<uint32_t>(seqNum);
            Return<void> hResult = mPlugin->setSharedBufferBase(hidl_memory(), bufferId);
            ALOGE_IF(!hResult.isOk(), "setSharedBufferBase(): remote call failed");
        }
        mHeapSizes.removeItem(seqNum);
    }
}

status_t CryptoHalHidl::checkSharedBuffer(const ::SharedBuffer& buffer) {
    int32_t seqNum = static_cast<int32_t>(buffer.bufferId);
    // memory must be in one of the heaps that have been set
    if (mHeapSizes.indexOfKey(seqNum) < 0) {
        return UNKNOWN_ERROR;
    }

    // memory must be within the address space of the heap
    size_t heapSize = mHeapSizes.valueFor(seqNum);
    if (heapSize < buffer.offset + buffer.size || SIZE_MAX - buffer.offset < buffer.size) {
        android_errorWriteLog(0x534e4554, "76221123");
        return UNKNOWN_ERROR;
    }

    return OK;
}

ssize_t CryptoHalHidl::decrypt(const uint8_t keyId[16], const uint8_t iv[16],
                               CryptoPlugin::Mode mode, const CryptoPlugin::Pattern& pattern,
                               const drm::V1_0::SharedBuffer& hSource, size_t offset,
                               const CryptoPlugin::SubSample* subSamples, size_t numSubSamples,
                               const drm::V1_0::DestinationBuffer& hDestination,
                               AString* errorDetailMsg) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    Mode hMode;
    switch (mode) {
        case CryptoPlugin::kMode_Unencrypted:
            hMode = Mode::UNENCRYPTED;
            break;
        case CryptoPlugin::kMode_AES_CTR:
            hMode = Mode::AES_CTR;
            break;
        case CryptoPlugin::kMode_AES_WV:
            hMode = Mode::AES_CBC_CTS;
            break;
        case CryptoPlugin::kMode_AES_CBC:
            hMode = Mode::AES_CBC;
            break;
        default:
            return UNKNOWN_ERROR;
    }

    Pattern hPattern;
    hPattern.encryptBlocks = pattern.mEncryptBlocks;
    hPattern.skipBlocks = pattern.mSkipBlocks;

    std::vector<SubSample> stdSubSamples;
    for (size_t i = 0; i < numSubSamples; i++) {
        SubSample subSample;
        subSample.numBytesOfClearData = subSamples[i].mNumBytesOfClearData;
        subSample.numBytesOfEncryptedData = subSamples[i].mNumBytesOfEncryptedData;
        stdSubSamples.push_back(subSample);
    }
    auto hSubSamples = hidl_vec<SubSample>(stdSubSamples);

    bool secure;
    if (hDestination.type == BufferType::SHARED_MEMORY) {
        status_t status = checkSharedBuffer(hDestination.nonsecureMemory);
        if (status != OK) {
            return status;
        }
        secure = false;
    } else if (hDestination.type == BufferType::NATIVE_HANDLE) {
        secure = true;
    } else {
        android_errorWriteLog(0x534e4554, "70526702");
        return UNKNOWN_ERROR;
    }

    status_t status = checkSharedBuffer(hSource);
    if (status != OK) {
        return status;
    }

    status_t err = UNKNOWN_ERROR;
    uint32_t bytesWritten = 0;

    Return<void> hResult;

    mLock.unlock();
    if (mPluginV1_2 != NULL) {
        hResult = mPluginV1_2->decrypt_1_2(
                secure, toHidlArray16(keyId), toHidlArray16(iv), hMode, hPattern, hSubSamples,
                hSource, offset, hDestination,
                [&](Status_V1_2 status, uint32_t hBytesWritten, hidl_string hDetailedError) {
                    if (status == Status_V1_2::OK) {
                        bytesWritten = hBytesWritten;
                        if (errorDetailMsg != nullptr) {
                            *errorDetailMsg = toString8(hDetailedError);
                        }
                    }
                    err = toStatusT(status);
                });
    } else {
        hResult = mPlugin->decrypt(
                secure, toHidlArray16(keyId), toHidlArray16(iv), hMode, hPattern, hSubSamples,
                hSource, offset, hDestination,
                [&](Status status, uint32_t hBytesWritten, hidl_string hDetailedError) {
                    if (status == Status::OK) {
                        bytesWritten = hBytesWritten;
                        if (errorDetailMsg != nullptr) {
                            *errorDetailMsg = toString8(hDetailedError);
                        }
                    }
                    err = toStatusT(status);
                });
    }

    err = hResult.isOk() ? err : DEAD_OBJECT;
    if (err == OK) {
        return bytesWritten;
    }
    return err;
}

void CryptoHalHidl::notifyResolution(uint32_t width, uint32_t height) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return;
    }

    auto hResult = mPlugin->notifyResolution(width, height);
    ALOGE_IF(!hResult.isOk(), "notifyResolution txn failed %s", hResult.description().c_str());
}

DrmStatus CryptoHalHidl::setMediaDrmSession(const Vector<uint8_t>& sessionId) {
    Mutex::Autolock autoLock(mLock);

    if (mInitCheck != OK) {
        return mInitCheck;
    }

    auto err = mPlugin->setMediaDrmSession(toHidlVec(sessionId));
    return err.isOk() ? toStatusT(err) : DEAD_OBJECT;
}

status_t CryptoHalHidl::getLogMessages(Vector<drm::V1_4::LogMessage>& logs) const {
    Mutex::Autolock autoLock(mLock);
    return DrmUtils::GetLogMessages<drm::V1_4::ICryptoPlugin>(mPlugin, logs);
}
}  // namespace android
