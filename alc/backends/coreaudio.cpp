/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "coreaudio.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmath>

#include "alnumeric.h"
#include "core/converter.h"
#include "core/device.h"
#include "core/logging.h"
#include "ringbuffer.h"

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>


namespace {

static const char ca_device[] = "CoreAudio Default";


struct CoreAudioPlayback final : public BackendBase {
    CoreAudioPlayback(DeviceBase *device) noexcept : BackendBase{device} { }
    ~CoreAudioPlayback() override;

    OSStatus MixerProc(AudioUnitRenderActionFlags *ioActionFlags,
        const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames,
        AudioBufferList *ioData) noexcept;
    static OSStatus MixerProcC(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
        const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames,
        AudioBufferList *ioData) noexcept
    {
        return static_cast<CoreAudioPlayback*>(inRefCon)->MixerProc(ioActionFlags, inTimeStamp,
            inBusNumber, inNumberFrames, ioData);
    }

    void open(const char *name) override;
    bool reset() override;
    void start() override;
    void stop() override;

    AudioUnit mAudioUnit{};

    uint mFrameSize{0u};
    AudioStreamBasicDescription mFormat{}; // This is the OpenAL format as a CoreAudio ASBD

    DEF_NEWDEL(CoreAudioPlayback)
};

CoreAudioPlayback::~CoreAudioPlayback()
{
    AudioUnitUninitialize(mAudioUnit);
    AudioComponentInstanceDispose(mAudioUnit);
}


OSStatus CoreAudioPlayback::MixerProc(AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
    UInt32, AudioBufferList *ioData) noexcept
{
    for(size_t i{0};i < ioData->mNumberBuffers;++i)
    {
        auto &buffer = ioData->mBuffers[i];
        mDevice->renderSamples(buffer.mData, buffer.mDataByteSize/mFrameSize,
            buffer.mNumberChannels);
    }
    return noErr;
}


void CoreAudioPlayback::open(const char *name)
{
    if(!name)
        name = ca_device;
    else if(strcmp(name, ca_device) != 0)
        throw al::backend_exception{al::backend_error::NoDevice, "Device name \"%s\" not found",
            name};

    /* open the default output unit */
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_IOS
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
#endif
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    AudioComponent comp{AudioComponentFindNext(NULL, &desc)};
    if(comp == nullptr)
        throw al::backend_exception{al::backend_error::NoDevice, "Could not find audio component"};

    AudioUnit audioUnit{};
    OSStatus err{AudioComponentInstanceNew(comp, &audioUnit)};
    if(err != noErr)
        throw al::backend_exception{al::backend_error::NoDevice,
            "Could not create component instance: %u", err};

    err = AudioUnitInitialize(audioUnit);
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not initialize audio unit: %u", err};

    /* WARNING: I don't know if "valid" audio unit values are guaranteed to be
     * non-0. If not, this logic is broken.
     */
    if(mAudioUnit)
    {
        AudioUnitUninitialize(mAudioUnit);
        AudioComponentInstanceDispose(mAudioUnit);
    }
    mAudioUnit = audioUnit;

    mDevice->DeviceName = name;
}

bool CoreAudioPlayback::reset()
{
    OSStatus err{AudioUnitUninitialize(mAudioUnit)};
    if(err != noErr)
        ERR("-- AudioUnitUninitialize failed.\n");

    /* retrieve default output unit's properties (output side) */
    AudioStreamBasicDescription streamFormat{};
    auto size = static_cast<UInt32>(sizeof(AudioStreamBasicDescription));
    err = AudioUnitGetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
        0, &streamFormat, &size);
    if(err != noErr || size != sizeof(AudioStreamBasicDescription))
    {
        ERR("AudioUnitGetProperty failed\n");
        return false;
    }

#if 0
    TRACE("Output streamFormat of default output unit -\n");
    TRACE("  streamFormat.mFramesPerPacket = %d\n", streamFormat.mFramesPerPacket);
    TRACE("  streamFormat.mChannelsPerFrame = %d\n", streamFormat.mChannelsPerFrame);
    TRACE("  streamFormat.mBitsPerChannel = %d\n", streamFormat.mBitsPerChannel);
    TRACE("  streamFormat.mBytesPerPacket = %d\n", streamFormat.mBytesPerPacket);
    TRACE("  streamFormat.mBytesPerFrame = %d\n", streamFormat.mBytesPerFrame);
    TRACE("  streamFormat.mSampleRate = %5.0f\n", streamFormat.mSampleRate);
#endif

    /* set default output unit's input side to match output side */
    err = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
        0, &streamFormat, size);
    if(err != noErr)
    {
        ERR("AudioUnitSetProperty failed\n");
        return false;
    }

    if(mDevice->Frequency != streamFormat.mSampleRate)
    {
        mDevice->BufferSize = static_cast<uint>(uint64_t{mDevice->BufferSize} *
            streamFormat.mSampleRate / mDevice->Frequency);
        mDevice->Frequency = static_cast<uint>(streamFormat.mSampleRate);
    }

    /* FIXME: How to tell what channels are what in the output device, and how
     * to specify what we're giving?  eg, 6.0 vs 5.1 */
    switch(streamFormat.mChannelsPerFrame)
    {
        case 1:
            mDevice->FmtChans = DevFmtMono;
            break;
        case 2:
            mDevice->FmtChans = DevFmtStereo;
            break;
        case 4:
            mDevice->FmtChans = DevFmtQuad;
            break;
        case 6:
            mDevice->FmtChans = DevFmtX51;
            break;
        case 7:
            mDevice->FmtChans = DevFmtX61;
            break;
        case 8:
            mDevice->FmtChans = DevFmtX71;
            break;
        default:
            ERR("Unhandled channel count (%d), using Stereo\n", streamFormat.mChannelsPerFrame);
            mDevice->FmtChans = DevFmtStereo;
            streamFormat.mChannelsPerFrame = 2;
            break;
    }
    setDefaultWFXChannelOrder();

    /* use channel count and sample rate from the default output unit's current
     * parameters, but reset everything else */
    streamFormat.mFramesPerPacket = 1;
    streamFormat.mFormatFlags = 0;
    switch(mDevice->FmtType)
    {
        case DevFmtUByte:
            mDevice->FmtType = DevFmtByte;
            /* fall-through */
        case DevFmtByte:
            streamFormat.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger;
            streamFormat.mBitsPerChannel = 8;
            break;
        case DevFmtUShort:
            mDevice->FmtType = DevFmtShort;
            /* fall-through */
        case DevFmtShort:
            streamFormat.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger;
            streamFormat.mBitsPerChannel = 16;
            break;
        case DevFmtUInt:
            mDevice->FmtType = DevFmtInt;
            /* fall-through */
        case DevFmtInt:
            streamFormat.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger;
            streamFormat.mBitsPerChannel = 32;
            break;
        case DevFmtFloat:
            streamFormat.mFormatFlags = kLinearPCMFormatFlagIsFloat;
            streamFormat.mBitsPerChannel = 32;
            break;
    }
    streamFormat.mBytesPerFrame = streamFormat.mChannelsPerFrame *
                                  streamFormat.mBitsPerChannel / 8;
    streamFormat.mBytesPerPacket = streamFormat.mBytesPerFrame;
    streamFormat.mFormatID = kAudioFormatLinearPCM;
    streamFormat.mFormatFlags |= kAudioFormatFlagsNativeEndian |
                                 kLinearPCMFormatFlagIsPacked;

    err = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
        0, &streamFormat, sizeof(AudioStreamBasicDescription));
    if(err != noErr)
    {
        ERR("AudioUnitSetProperty failed\n");
        return false;
    }

    /* setup callback */
    mFrameSize = mDevice->frameSizeFromFmt();
    AURenderCallbackStruct input{};
    input.inputProc = CoreAudioPlayback::MixerProcC;
    input.inputProcRefCon = this;

    err = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_SetRenderCallback,
        kAudioUnitScope_Input, 0, &input, sizeof(AURenderCallbackStruct));
    if(err != noErr)
    {
        ERR("AudioUnitSetProperty failed\n");
        return false;
    }

    /* init the default audio unit... */
    err = AudioUnitInitialize(mAudioUnit);
    if(err != noErr)
    {
        ERR("AudioUnitInitialize failed\n");
        return false;
    }

    return true;
}

void CoreAudioPlayback::start()
{
    const OSStatus err{AudioOutputUnitStart(mAudioUnit)};
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "AudioOutputUnitStart failed: %d", err};
}

void CoreAudioPlayback::stop()
{
    OSStatus err{AudioOutputUnitStop(mAudioUnit)};
    if(err != noErr)
        ERR("AudioOutputUnitStop failed\n");
}


struct CoreAudioCapture final : public BackendBase {
    CoreAudioCapture(DeviceBase *device) noexcept : BackendBase{device} { }
    ~CoreAudioCapture() override;

    OSStatus RecordProc(AudioUnitRenderActionFlags *ioActionFlags,
        const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
        UInt32 inNumberFrames, AudioBufferList *ioData) noexcept;
    static OSStatus RecordProcC(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
        const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames,
        AudioBufferList *ioData) noexcept
    {
        return static_cast<CoreAudioCapture*>(inRefCon)->RecordProc(ioActionFlags, inTimeStamp,
            inBusNumber, inNumberFrames, ioData);
    }

    void open(const char *name) override;
    void start() override;
    void stop() override;
    void captureSamples(al::byte *buffer, uint samples) override;
    uint availableSamples() override;

    AudioUnit mAudioUnit{0};

    uint mFrameSize{0u};
    AudioStreamBasicDescription mFormat{};  // This is the OpenAL format as a CoreAudio ASBD

    SampleConverterPtr mConverter;

    RingBufferPtr mRing{nullptr};

    DEF_NEWDEL(CoreAudioCapture)
};

CoreAudioCapture::~CoreAudioCapture()
{
    if(mAudioUnit)
        AudioComponentInstanceDispose(mAudioUnit);
    mAudioUnit = 0;
}


OSStatus CoreAudioCapture::RecordProc(AudioUnitRenderActionFlags*,
    const AudioTimeStamp *inTimeStamp, UInt32, UInt32 inNumberFrames,
    AudioBufferList*) noexcept
{
    AudioUnitRenderActionFlags flags = 0;
    union {
        al::byte _[sizeof(AudioBufferList) + sizeof(AudioBuffer)*2];
        AudioBufferList list;
    } audiobuf{};

    auto rec_vec = mRing->getWriteVector();
    inNumberFrames = static_cast<UInt32>(minz(inNumberFrames,
        rec_vec.first.len+rec_vec.second.len));

    // Fill the ringbuffer's two segments with data from the input device
    if(rec_vec.first.len >= inNumberFrames)
    {
        audiobuf.list.mNumberBuffers = 1;
        audiobuf.list.mBuffers[0].mNumberChannels = mFormat.mChannelsPerFrame;
        audiobuf.list.mBuffers[0].mData = rec_vec.first.buf;
        audiobuf.list.mBuffers[0].mDataByteSize = inNumberFrames * mFormat.mBytesPerFrame;
    }
    else
    {
        const auto remaining = static_cast<uint>(inNumberFrames - rec_vec.first.len);
        audiobuf.list.mNumberBuffers = 2;
        audiobuf.list.mBuffers[0].mNumberChannels = mFormat.mChannelsPerFrame;
        audiobuf.list.mBuffers[0].mData = rec_vec.first.buf;
        audiobuf.list.mBuffers[0].mDataByteSize = static_cast<UInt32>(rec_vec.first.len) *
            mFormat.mBytesPerFrame;
        audiobuf.list.mBuffers[1].mNumberChannels = mFormat.mChannelsPerFrame;
        audiobuf.list.mBuffers[1].mData = rec_vec.second.buf;
        audiobuf.list.mBuffers[1].mDataByteSize = remaining * mFormat.mBytesPerFrame;
    }
    OSStatus err{AudioUnitRender(mAudioUnit, &flags, inTimeStamp, audiobuf.list.mNumberBuffers,
        inNumberFrames, &audiobuf.list)};
    if(err != noErr)
    {
        ERR("AudioUnitRender error: %d\n", err);
        return err;
    }

    mRing->writeAdvance(inNumberFrames);
    return noErr;
}


void CoreAudioCapture::open(const char *name)
{
    AudioStreamBasicDescription requestedFormat;  // The application requested format
    AudioStreamBasicDescription hardwareFormat;   // The hardware format
    AudioStreamBasicDescription outputFormat;     // The AudioUnit output format
    AURenderCallbackStruct input;
    AudioComponentDescription desc;
    UInt32 propertySize;
    UInt32 enableIO;
    AudioComponent comp;
    OSStatus err;

    if(!name)
        name = ca_device;
    else if(strcmp(name, ca_device) != 0)
        throw al::backend_exception{al::backend_error::NoDevice, "Device name \"%s\" not found",
            name};

    desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_IOS
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
    desc.componentSubType = kAudioUnitSubType_HALOutput;
#endif
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    // Search for component with given description
    comp = AudioComponentFindNext(NULL, &desc);
    if(comp == NULL)
        throw al::backend_exception{al::backend_error::NoDevice, "Could not find audio component"};

    // Open the component
    err = AudioComponentInstanceNew(comp, &mAudioUnit);
    if(err != noErr)
        throw al::backend_exception{al::backend_error::NoDevice,
            "Could not create component instance: %u", err};

    // Turn off AudioUnit output
    enableIO = 0;
    err = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not disable audio unit output property: %u", err};

    // Turn on AudioUnit input
    enableIO = 1;
    err = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_EnableIO,
        kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not enable audio unit input property: %u", err};

#if !TARGET_OS_IOS
    {
        // Get the default input device
        AudioDeviceID inputDevice = kAudioDeviceUnknown;

        propertySize = sizeof(AudioDeviceID);
        AudioObjectPropertyAddress propertyAddress{};
        propertyAddress.mSelector = kAudioHardwarePropertyDefaultInputDevice;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
        propertyAddress.mElement = kAudioObjectPropertyElementMaster;

        err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, nullptr,
            &propertySize, &inputDevice);
        if(err != noErr)
            throw al::backend_exception{al::backend_error::NoDevice,
                "Could not get input device: %u", err};
        if(inputDevice == kAudioDeviceUnknown)
            throw al::backend_exception{al::backend_error::NoDevice, "Unknown input device"};

        // Track the input device
        err = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global, 0, &inputDevice, sizeof(AudioDeviceID));
        if(err != noErr)
            throw al::backend_exception{al::backend_error::NoDevice,
                "Could not set input device: %u", err};
    }
#endif

    // set capture callback
    input.inputProc = CoreAudioCapture::RecordProcC;
    input.inputProcRefCon = this;

    err = AudioUnitSetProperty(mAudioUnit, kAudioOutputUnitProperty_SetInputCallback,
        kAudioUnitScope_Global, 0, &input, sizeof(AURenderCallbackStruct));
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not set capture callback: %u", err};

    // Disable buffer allocation for capture
    UInt32 flag{0};
    err = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_ShouldAllocateBuffer,
        kAudioUnitScope_Output, 1, &flag, sizeof(flag));
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not disable buffer allocation property: %u", err};

    // Initialize the device
    err = AudioUnitInitialize(mAudioUnit);
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not initialize audio unit: %u", err};

    // Get the hardware format
    propertySize = sizeof(AudioStreamBasicDescription);
    err = AudioUnitGetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
        1, &hardwareFormat, &propertySize);
    if(err != noErr || propertySize != sizeof(AudioStreamBasicDescription))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not get input format: %u", err};

    // Set up the requested format description
    switch(mDevice->FmtType)
    {
    case DevFmtByte:
        requestedFormat.mBitsPerChannel = 8;
        requestedFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        break;
    case DevFmtUByte:
        requestedFormat.mBitsPerChannel = 8;
        requestedFormat.mFormatFlags = kAudioFormatFlagIsPacked;
        break;
    case DevFmtShort:
        requestedFormat.mBitsPerChannel = 16;
        requestedFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
        break;
    case DevFmtUShort:
        requestedFormat.mBitsPerChannel = 16;
        requestedFormat.mFormatFlags = kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
        break;
    case DevFmtInt:
        requestedFormat.mBitsPerChannel = 32;
        requestedFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
        break;
    case DevFmtUInt:
        requestedFormat.mBitsPerChannel = 32;
        requestedFormat.mFormatFlags = kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
        break;
    case DevFmtFloat:
        requestedFormat.mBitsPerChannel = 32;
        requestedFormat.mFormatFlags = kLinearPCMFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
        break;
    }

    switch(mDevice->FmtChans)
    {
    case DevFmtMono:
        requestedFormat.mChannelsPerFrame = 1;
        break;
    case DevFmtStereo:
        requestedFormat.mChannelsPerFrame = 2;
        break;

    case DevFmtQuad:
    case DevFmtX51:
    case DevFmtX51Rear:
    case DevFmtX61:
    case DevFmtX71:
    case DevFmtAmbi3D:
        throw al::backend_exception{al::backend_error::DeviceError, "%s not supported",
            DevFmtChannelsString(mDevice->FmtChans)};
    }

    requestedFormat.mBytesPerFrame = requestedFormat.mChannelsPerFrame * requestedFormat.mBitsPerChannel / 8;
    requestedFormat.mBytesPerPacket = requestedFormat.mBytesPerFrame;
    requestedFormat.mSampleRate = mDevice->Frequency;
    requestedFormat.mFormatID = kAudioFormatLinearPCM;
    requestedFormat.mReserved = 0;
    requestedFormat.mFramesPerPacket = 1;

    // save requested format description for later use
    mFormat = requestedFormat;
    mFrameSize = mDevice->frameSizeFromFmt();

    // Use intermediate format for sample rate conversion (outputFormat)
    // Set sample rate to the same as hardware for resampling later
    outputFormat = requestedFormat;
    outputFormat.mSampleRate = hardwareFormat.mSampleRate;

    // The output format should be the requested format, but using the hardware sample rate
    // This is because the AudioUnit will automatically scale other properties, except for sample rate
    err = AudioUnitSetProperty(mAudioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
        1, &outputFormat, sizeof(outputFormat));
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not set input format: %u", err};

    /* Calculate the minimum AudioUnit output format frame count for the pre-
     * conversion ring buffer. Ensure at least 100ms for the total buffer.
     */
    double srateScale{double{outputFormat.mSampleRate} / mDevice->Frequency};
    auto FrameCount64 = maxu64(static_cast<uint64_t>(std::ceil(mDevice->BufferSize*srateScale)),
        static_cast<UInt32>(outputFormat.mSampleRate)/10);
    FrameCount64 += MaxResamplerPadding;
    if(FrameCount64 > std::numeric_limits<int32_t>::max())
        throw al::backend_exception{al::backend_error::DeviceError,
            "Calculated frame count is too large: %" PRIu64, FrameCount64};

    UInt32 outputFrameCount{};
    propertySize = sizeof(outputFrameCount);
    err = AudioUnitGetProperty(mAudioUnit, kAudioUnitProperty_MaximumFramesPerSlice,
        kAudioUnitScope_Global, 0, &outputFrameCount, &propertySize);
    if(err != noErr || propertySize != sizeof(outputFrameCount))
        throw al::backend_exception{al::backend_error::DeviceError,
            "Could not get input frame count: %u", err};

    outputFrameCount = static_cast<UInt32>(maxu64(outputFrameCount, FrameCount64));
    mRing = RingBuffer::Create(outputFrameCount, mFrameSize, false);

    /* Set up sample converter if needed */
    if(outputFormat.mSampleRate != mDevice->Frequency)
        mConverter = CreateSampleConverter(mDevice->FmtType, mDevice->FmtType,
            mFormat.mChannelsPerFrame, static_cast<uint>(hardwareFormat.mSampleRate),
            mDevice->Frequency, Resampler::FastBSinc24);

    mDevice->DeviceName = name;
}


void CoreAudioCapture::start()
{
    OSStatus err{AudioOutputUnitStart(mAudioUnit)};
    if(err != noErr)
        throw al::backend_exception{al::backend_error::DeviceError,
            "AudioOutputUnitStart failed: %d", err};
}

void CoreAudioCapture::stop()
{
    OSStatus err{AudioOutputUnitStop(mAudioUnit)};
    if(err != noErr)
        ERR("AudioOutputUnitStop failed\n");
}

void CoreAudioCapture::captureSamples(al::byte *buffer, uint samples)
{
    if(!mConverter)
    {
        mRing->read(buffer, samples);
        return;
    }

    auto rec_vec = mRing->getReadVector();
    const void *src0{rec_vec.first.buf};
    auto src0len = static_cast<uint>(rec_vec.first.len);
    uint got{mConverter->convert(&src0, &src0len, buffer, samples)};
    size_t total_read{rec_vec.first.len - src0len};
    if(got < samples && !src0len && rec_vec.second.len > 0)
    {
        const void *src1{rec_vec.second.buf};
        auto src1len = static_cast<uint>(rec_vec.second.len);
        got += mConverter->convert(&src1, &src1len, buffer + got*mFrameSize, samples-got);
        total_read += rec_vec.second.len - src1len;
    }

    mRing->readAdvance(total_read);
}

uint CoreAudioCapture::availableSamples()
{
    if(!mConverter) return static_cast<uint>(mRing->readSpace());
    return mConverter->availableOut(static_cast<uint>(mRing->readSpace()));
}

} // namespace

BackendFactory &CoreAudioBackendFactory::getFactory()
{
    static CoreAudioBackendFactory factory{};
    return factory;
}

bool CoreAudioBackendFactory::init() { return true; }

bool CoreAudioBackendFactory::querySupport(BackendType type)
{ return type == BackendType::Playback || type == BackendType::Capture; }

std::string CoreAudioBackendFactory::probe(BackendType type)
{
    std::string outnames;
    switch(type)
    {
    case BackendType::Playback:
    case BackendType::Capture:
        /* Includes null char. */
        outnames.append(ca_device, sizeof(ca_device));
        break;
    }
    return outnames;
}

BackendPtr CoreAudioBackendFactory::createBackend(DeviceBase *device, BackendType type)
{
    if(type == BackendType::Playback)
        return BackendPtr{new CoreAudioPlayback{device}};
    if(type == BackendType::Capture)
        return BackendPtr{new CoreAudioCapture{device}};
    return nullptr;
}
