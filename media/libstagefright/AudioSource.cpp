/*
 * Copyright (C) 2010 The Android Open Source Project
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
#define LOG_TAG "AudioSource"
#include <utils/Log.h>

#include <media/stagefright/AudioSource.h>

#include <media/AudioRecord.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MetaData.h>
//#include <media/stagefright/foundation/ADebug.h>
#include <cutils/properties.h>
#include <stdlib.h>

namespace android {
static void AudioRecordCallbackFunction(int event, void *user, void *info) {
    AudioSource *source = (AudioSource *) user;
    switch (event) {
        case AudioRecord::EVENT_MORE_DATA: {
            source->dataCallbackTimestamp(*((AudioRecord::Buffer *) info), systemTime() / 1000);
            break;
        }
        case AudioRecord::EVENT_OVERRUN: {
            LOGW("AudioRecord reported overrun!");
            break;
        }
        default:
            // does nothing
            break;
    }
}

AudioSource::AudioSource(
        int inputSource, uint32_t sampleRate, uint32_t channels)
    : mStarted(false),
      mCollectStats(false),
      mSampleRate(sampleRate),
      mPrevSampleTimeUs(0),
      mTotalLostFrames(0),
      mPrevLostBytes(0),
      mNumFramesReceived(0),
      mGroup(NULL), 
      mNumClientOwnedBuffers(0){ 
    LOGV("sampleRate: %d, channels: %d", sampleRate, channels);
    CHECK(channels == 1 || channels == 2);
    uint32_t flags = AudioRecord::RECORD_AGC_ENABLE |
                     AudioRecord::RECORD_NS_ENABLE  |
                     AudioRecord::RECORD_IIR_ENABLE;

    mRecord = new AudioRecord(
                inputSource, sampleRate, AudioSystem::PCM_16_BIT,
                channels > 1? AudioSystem::CHANNEL_IN_STEREO: AudioSystem::CHANNEL_IN_MONO,
                4 * kMaxBufferSize / sizeof(int16_t), /* Enable ping-pong buffers */
                flags,
                AudioRecordCallbackFunction,
                this);

    mInitCheck = mRecord->initCheck();
}

AudioSource::~AudioSource() {
    if (mStarted) {
        stop();
    }

    delete mRecord;
    mRecord = NULL;
}

status_t AudioSource::initCheck() const {
    return mInitCheck;
}

status_t AudioSource::start(MetaData *params) {
    Mutex::Autolock autoLock(mLock);
    if (mStarted) {
        return UNKNOWN_ERROR;
    }

    if (mInitCheck != OK) {
        return NO_INIT;
    }

    char value[PROPERTY_VALUE_MAX];
    if (property_get("media.stagefright.record-stats", value, NULL)
        && (!strcmp(value, "1") || !strcasecmp(value, "true"))) {
        mCollectStats = true;
    }

    mTrackMaxAmplitude = false;
    mMaxAmplitude = 0;
    mInitialReadTimeUs = 0;
    mStartTimeUs = 0;
    int64_t startTimeUs;
    if (params && params->findInt64(kKeyTime, &startTimeUs)) {
        mStartTimeUs = startTimeUs;
    }
    status_t err = mRecord->start();

    if (err == OK) {
        mGroup = new MediaBufferGroup;
        mGroup->add_buffer(new MediaBuffer(kMaxBufferSize));

        mStarted = true;
    }

    return err;
}

void AudioSource::releaseQueuedFrames_l() {
    LOGV("releaseQueuedFrames_l");
    List<MediaBuffer *>::iterator it;
    while (!mBuffersReceived.empty()) {
        it = mBuffersReceived.begin();
        (*it)->release();
        mBuffersReceived.erase(it);
    }
}

void AudioSource::waitOutstandingEncodingFrames_l() {
    LOGV("waitOutstandingEncodingFrames_l: %lld", mNumClientOwnedBuffers);
    while (mNumClientOwnedBuffers > 0) {
        mFrameEncodingCompletionCondition.wait(mLock);
    }
}
status_t AudioSource::stop() {
    Mutex::Autolock autoLock(mLock);
    if (!mStarted) {
        return UNKNOWN_ERROR;
    }

    if (mInitCheck != OK) {
        return NO_INIT;
    }

    mStarted = false;
    mRecord->stop();

    waitOutstandingEncodingFrames_l();
    releaseQueuedFrames_l();

    return OK;
}

sp<MetaData> AudioSource::getFormat() {
    Mutex::Autolock autoLock(mLock);
    if (mInitCheck != OK) {
        return 0;
    }

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RAW);
    meta->setInt32(kKeySampleRate, mRecord->getSampleRate());
    meta->setInt32(kKeyChannelCount, mRecord->channelCount());
    meta->setInt32(kKeyMaxInputSize, kMaxBufferSize);

    return meta;
}

/*
 * Returns -1 if frame skipping request is too long.
 * Returns  0 if there is no need to skip frames.
 * Returns  1 if we need to skip frames.
 */
static int skipFrame(int64_t timestampUs,
        const MediaSource::ReadOptions *options) {

    int64_t skipFrameUs;
    if (!options || !options->getSkipFrame(&skipFrameUs)) {
        return 0;
    }

    if (skipFrameUs <= timestampUs) {
        return 0;
    }

    // Safe guard against the abuse of the kSkipFrame_Option.
    if (skipFrameUs - timestampUs >= 1E6) {
        LOGE("Frame skipping requested is way too long: %lld us",
            skipFrameUs - timestampUs);

        return -1;
    }

    LOGV("skipFrame: %lld us > timestamp: %lld us",
        skipFrameUs, timestampUs);

    return 1;

}

void AudioSource::rampVolume(
        int32_t startFrame, int32_t rampDurationFrames,
        uint8_t *data,   size_t bytes) {

    const int32_t kShift = 14;
    int32_t fixedMultiplier = (startFrame << kShift) / rampDurationFrames;
    const int32_t nChannels = mRecord->channelCount();
    int32_t stopFrame = startFrame + bytes / sizeof(int16_t);
    int16_t *frame = (int16_t *) data;
    if (stopFrame > rampDurationFrames) {
        stopFrame = rampDurationFrames;
    }

    while (startFrame < stopFrame) {
        if (nChannels == 1) {  // mono
            frame[0] = (frame[0] * fixedMultiplier) >> kShift;
            ++frame;
            ++startFrame;
        } else {               // stereo
            frame[0] = (frame[0] * fixedMultiplier) >> kShift;
            frame[1] = (frame[1] * fixedMultiplier) >> kShift;
            frame += 2;
            startFrame += 2;
        }

        // Update the multiplier every 4 frames
        if ((startFrame & 3) == 0) {
            fixedMultiplier = (startFrame << kShift) / rampDurationFrames;
        }
    }
}

status_t AudioSource::read(
        MediaBuffer **out, const ReadOptions *options) {
    Mutex::Autolock autoLock(mLock);
    *out = NULL;

    if (mInitCheck != OK) {
        return NO_INIT;
    }

    int64_t readTimeUs = systemTime() / 1000;
    *out = NULL;

    MediaBuffer *buffer;
    CHECK_EQ(mGroup->acquire_buffer(&buffer), OK);

    int err = 0;
    while (mStarted) {

        uint32_t numFramesRecorded;
        mRecord->getPosition(&numFramesRecorded);


        if (numFramesRecorded == 0 && mPrevSampleTimeUs == 0) {
            mInitialReadTimeUs = readTimeUs;
            // Initial delay
            if (mStartTimeUs > 0) {
                mStartTimeUs = readTimeUs - mStartTimeUs;
            } else {
                // Assume latency is constant.
                mStartTimeUs += mRecord->latency() * 1000;
            }
            mPrevSampleTimeUs = mStartTimeUs;
        }

        uint32_t sampleRate = mRecord->getSampleRate();

        // Insert null frames when lost frames are detected.
        int64_t timestampUs = mPrevSampleTimeUs;
        uint32_t numLostBytes = mRecord->getInputFramesLost() << 1;
        numLostBytes += mPrevLostBytes;
#if 0
        // Simulate lost frames
        numLostBytes = ((rand() * 1.0 / RAND_MAX)) * 2 * kMaxBufferSize;
        numLostBytes &= 0xFFFFFFFE; // Alignment requirement

        // Reduce the chance to lose
        if (rand() * 1.0 / RAND_MAX >= 0.05) {
            numLostBytes = 0;
        }
#endif
        if (numLostBytes > 0) {
            if (numLostBytes > kMaxBufferSize) {
                mPrevLostBytes = numLostBytes - kMaxBufferSize;
                numLostBytes = kMaxBufferSize;
            } else {
                mPrevLostBytes = 0;
            }

            CHECK_EQ(numLostBytes & 1, 0);
            timestampUs += ((1000000LL * (numLostBytes >> 1)) +
                    (sampleRate >> 1)) / sampleRate;

            CHECK(timestampUs > mPrevSampleTimeUs);
            if (mCollectStats) {
                mTotalLostFrames += (numLostBytes >> 1);
            }
            if ((err = skipFrame(timestampUs, options)) == -1) {
                buffer->release();
                return UNKNOWN_ERROR;
            } else if (err != 0) {
                continue;
            }
            memset(buffer->data(), 0, numLostBytes);
            buffer->set_range(0, numLostBytes);
            if (numFramesRecorded == 0) {
                buffer->meta_data()->setInt64(kKeyAnchorTime, mStartTimeUs);
            }
            buffer->meta_data()->setInt64(kKeyTime, mStartTimeUs + mPrevSampleTimeUs);
            buffer->meta_data()->setInt64(kKeyDriftTime, readTimeUs - mInitialReadTimeUs);
            mPrevSampleTimeUs = timestampUs;
            *out = buffer;
            return OK;
        }

        ssize_t n = mRecord->read(buffer->data(), buffer->size());
        if (n <= 0) {
            LOGE("Read from AudioRecord returns: %ld", n);
            buffer->release();
            return UNKNOWN_ERROR;
        }

        int64_t recordDurationUs = (1000000LL * n >> 1) / sampleRate;
        timestampUs += recordDurationUs;
        if ((err = skipFrame(timestampUs, options)) == -1) {
            buffer->release();
            return UNKNOWN_ERROR;
        } else if (err != 0) {
            continue;
        }

        if (mPrevSampleTimeUs - mStartTimeUs < kAutoRampStartUs) {
            // Mute the initial video recording signal
            memset((uint8_t *) buffer->data(), 0, n);
        } else if (mPrevSampleTimeUs - mStartTimeUs < kAutoRampStartUs + kAutoRampDurationUs) {
            int32_t autoRampDurationFrames =
                    (kAutoRampDurationUs * sampleRate + 500000LL) / 1000000LL;

            int32_t autoRampStartFrames =
                    (kAutoRampStartUs * sampleRate + 500000LL) / 1000000LL;

            int32_t nFrames = numFramesRecorded - autoRampStartFrames;
            rampVolume(nFrames, autoRampDurationFrames, (uint8_t *) buffer->data(), n);
        }
        if (mTrackMaxAmplitude) {
            trackMaxAmplitude((int16_t *) buffer->data(), n >> 1);
        }

        if (numFramesRecorded == 0) {
            buffer->meta_data()->setInt64(kKeyAnchorTime, mStartTimeUs);
        }

        buffer->meta_data()->setInt64(kKeyTime, mStartTimeUs + mPrevSampleTimeUs);
        buffer->meta_data()->setInt64(kKeyDriftTime, readTimeUs - mInitialReadTimeUs);
        CHECK(timestampUs > mPrevSampleTimeUs);
        mPrevSampleTimeUs = timestampUs;
        LOGV("initial delay: %lld, sample rate: %d, timestamp: %lld",
                mStartTimeUs, sampleRate, timestampUs);

        buffer->set_range(0, n);

        *out = buffer;
        return OK;
    }

    return OK;
}


void AudioSource::signalBufferReturned(MediaBuffer *buffer) {
    LOGV("signalBufferReturned: %p", buffer->data());
    Mutex::Autolock autoLock(mLock);
    --mNumClientOwnedBuffers;
    buffer->setObserver(0);
    buffer->release();
    mFrameEncodingCompletionCondition.signal();
    return;
}


status_t AudioSource::dataCallbackTimestamp(
        const AudioRecord::Buffer& audioBuffer, int64_t timeUs) {
    LOGV("dataCallbackTimestamp: %lld us", timeUs);
    Mutex::Autolock autoLock(mLock);
    if (!mStarted) {
        LOGW("Spurious callback from AudioRecord. Drop the audio data.");
        return OK;
    }

    // Drop retrieved and previously lost audio data.
    if (mNumFramesReceived == 0 && timeUs < mStartTimeUs) {
        mRecord->getInputFramesLost();
        LOGV("Drop audio data at %lld/%lld us", timeUs, mStartTimeUs);
        return OK;
    }

    if (mNumFramesReceived == 0 && mPrevSampleTimeUs == 0) {
        mInitialReadTimeUs = timeUs;
        // Initial delay
        if (mStartTimeUs > 0) {
            mStartTimeUs = timeUs - mStartTimeUs;
        } else {
            // Assume latency is constant.
            mStartTimeUs += mRecord->latency() * 1000;
        }
        mPrevSampleTimeUs = mStartTimeUs;
    }

    int64_t timestampUs = mPrevSampleTimeUs;

    size_t numLostBytes = 0;
    if (mNumFramesReceived > 0) {  // Ignore earlier frame lost
        // getInputFramesLost() returns the number of lost frames.
        // Convert number of frames lost to number of bytes lost.
        numLostBytes = mRecord->getInputFramesLost() * mRecord->frameSize();
    }

    CHECK_EQ(numLostBytes & 1, 0u);
    CHECK_EQ(audioBuffer.size & 1, 0u);
    size_t bufferSize = numLostBytes + audioBuffer.size;
    MediaBuffer *buffer = new MediaBuffer(bufferSize);
    if (numLostBytes > 0) {
        memset(buffer->data(), 0, numLostBytes);
        memcpy((uint8_t *) buffer->data() + numLostBytes,
                    audioBuffer.i16, audioBuffer.size);
    } else {
        if (audioBuffer.size == 0) {
            LOGW("Nothing is available from AudioRecord callback buffer");
            buffer->release();
            return OK;
        }
        memcpy((uint8_t *) buffer->data(),
                audioBuffer.i16, audioBuffer.size);
    }


    buffer->set_range(0, bufferSize);
    timestampUs += ((1000000LL * (bufferSize >> 1)) +
                    (mSampleRate >> 1)) / mSampleRate;

    if (mNumFramesReceived == 0) {
        buffer->meta_data()->setInt64(kKeyAnchorTime, mStartTimeUs);
    }
    buffer->meta_data()->setInt64(kKeyTime, mPrevSampleTimeUs);
    buffer->meta_data()->setInt64(kKeyDriftTime, timeUs - mInitialReadTimeUs);
    mPrevSampleTimeUs = timestampUs;
    mNumFramesReceived += buffer->range_length() / sizeof(int16_t);
    mBuffersReceived.push_back(buffer);
    mFrameAvailableCondition.signal();

    return OK;
}



void AudioSource::trackMaxAmplitude(int16_t *data, int nSamples) {
    for (int i = nSamples; i > 0; --i) {
        int16_t value = *data++;
        if (value < 0) {
            value = -value;
        }
        if (mMaxAmplitude < value) {
            mMaxAmplitude = value;
        }
    }
}

int16_t AudioSource::getMaxAmplitude() {
    // First call activates the tracking.
    if (!mTrackMaxAmplitude) {
        mTrackMaxAmplitude = true;
    }
    int16_t value = mMaxAmplitude;
    mMaxAmplitude = 0;
    LOGV("max amplitude since last call: %d", value);
    return value;
}

}  // namespace android
