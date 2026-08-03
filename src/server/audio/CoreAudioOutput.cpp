#include "AudioOutput.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <spdlog/spdlog.h>

namespace creatures::audio {
namespace {

struct CoreAudioDevice {
    AudioDeviceID id;
    std::string name;
};

bool readProperty(AudioObjectID object, const AudioObjectPropertyAddress &address, void *value, UInt32 &size) {
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, value) == noErr;
}

std::string deviceName(AudioDeviceID device) {
    AudioObjectPropertyAddress address{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    if (!readProperty(device, address, &name, size) || name == nullptr) {
        return "Unnamed CoreAudio device";
    }
    std::array<char, 512> utf8{};
    const bool converted = CFStringGetCString(name, utf8.data(), utf8.size(), kCFStringEncodingUTF8);
    CFRelease(name);
    return converted ? std::string(utf8.data()) : "Unnamed CoreAudio device";
}

bool hasOutputChannels(AudioDeviceID device) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0) {
        return false;
    }
    std::vector<uint8_t> storage(size);
    auto *buffers = reinterpret_cast<AudioBufferList *>(storage.data());
    if (!readProperty(device, address, buffers, size)) {
        return false;
    }
    UInt32 channels = 0;
    for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
        channels += buffers->mBuffers[index].mNumberChannels;
    }
    return channels > 0;
}

std::vector<CoreAudioDevice> enumerateDevices() {
    AudioObjectPropertyAddress defaultAddress{kAudioHardwarePropertyDefaultOutputDevice,
                                              kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID defaultDevice = kAudioObjectUnknown;
    UInt32 defaultSize = sizeof(defaultDevice);
    readProperty(kAudioObjectSystemObject, defaultAddress, &defaultDevice, defaultSize);

    AudioObjectPropertyAddress devicesAddress{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devicesAddress, 0, nullptr, &size) != noErr) {
        return {};
    }
    std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
    if (!ids.empty() && !readProperty(kAudioObjectSystemObject, devicesAddress, ids.data(), size)) {
        return {};
    }
    std::vector<CoreAudioDevice> devices;
    if (defaultDevice != kAudioObjectUnknown && hasOutputChannels(defaultDevice)) {
        devices.push_back({defaultDevice, deviceName(defaultDevice)});
    }
    for (AudioDeviceID id : ids) {
        if (id != defaultDevice && hasOutputChannels(id)) {
            devices.push_back({id, deviceName(id)});
        }
    }
    return devices;
}

class CoreAudioOutput final : public AudioOutput {
  public:
    ~CoreAudioOutput() override { close(); }

    bool open(const NativeAudioConfig &config) override {
        close();
        sampleRate_ = config.sampleRate;
        channels_ = config.channels;
        if (sampleRate_ == 0 || channels_ == 0) {
            spdlog::error("Invalid CoreAudio output format: {} Hz, {} channels", sampleRate_, channels_);
            return false;
        }

        const auto devices = enumerateDevices();
        if (devices.empty()) {
            spdlog::error("CoreAudio reported no output devices");
            return false;
        }
        size_t deviceIndex = 0;
        if (config.deviceName.has_value()) {
            std::vector<size_t> matches;
            for (size_t index = 0; index < devices.size(); ++index) {
                if (devices[index].name == *config.deviceName) {
                    matches.push_back(index);
                }
            }
            if (matches.size() != 1) {
                spdlog::error("Configured CoreAudio output '{}' is {}", *config.deviceName,
                              matches.empty() ? "unavailable" : "ambiguous");
                return false;
            }
            deviceIndex = matches.front();
        }
        device_ = devices[deviceIndex].id;

        AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_HALOutput,
                                              kAudioUnitManufacturer_Apple, 0, 0};
        const AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (component == nullptr || !check(AudioComponentInstanceNew(component, &audioUnit_), "create output unit")) {
            close();
            return false;
        }
        UInt32 enabled = 1;
        UInt32 disabled = 0;
        if (!check(AudioUnitSetProperty(audioUnit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
                                        &enabled, sizeof(enabled)),
                   "enable output") ||
            !check(AudioUnitSetProperty(audioUnit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                                        &disabled, sizeof(disabled)),
                   "disable input") ||
            !check(AudioUnitSetProperty(audioUnit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                        &device_, sizeof(device_)),
                   "select output device")) {
            close();
            return false;
        }

        requestDevicePeriod();
        AudioStreamBasicDescription format{};
        format.mSampleRate = sampleRate_;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags =
            kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
        format.mBytesPerPacket = sizeof(int16_t) * channels_;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = sizeof(int16_t) * channels_;
        format.mChannelsPerFrame = channels_;
        format.mBitsPerChannel = 16;
        AURenderCallbackStruct callback{&CoreAudioOutput::renderCallback, this};
        if (!check(AudioUnitSetProperty(audioUnit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                        sizeof(format)),
                   "set output stream format") ||
            !check(AudioUnitSetProperty(audioUnit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                        &callback, sizeof(callback)),
                   "install output callback") ||
            !check(AudioUnitInitialize(audioUnit_), "initialize output unit")) {
            close();
            return false;
        }

        pipelineLatencyFrames_ = readPipelineLatency();
        const size_t queueTargetFrames =
            std::max<size_t>(1, static_cast<size_t>(sampleRate_) * NATIVE_AUDIO_QUEUE_TARGET_MS / 1000);
        const size_t ringFrames = std::max<size_t>(
            NATIVE_AUDIO_RING_FRAMES, queueTargetFrames + 4096 + devicePeriodFrames_ + pipelineLatencyFrames_ + 1);
        outputRing_.assign(ringFrames * channels_, 0);
        readPosition_.store(0);
        writePosition_.store(0);
        spdlog::info("CoreAudio output ready: device {} ('{}'), {} Hz, {} channels, period={} frames, pipeline={} "
                     "frames",
                     deviceIndex, devices[deviceIndex].name, sampleRate_, channels_, devicePeriodFrames_,
                     pipelineLatencyFrames_);
        return true;
    }

    bool start() override {
        if (audioUnit_ == nullptr) {
            return false;
        }
        running_.store(true);
        const OSStatus status = AudioOutputUnitStart(audioUnit_);
        if (status != noErr) {
            running_.store(false);
            spdlog::error("Unable to start CoreAudio output (status {})", status);
            return false;
        }
        return true;
    }

    void stopAndClear() override {
        running_.store(false);
        if (audioUnit_ != nullptr) {
            const OSStatus status = AudioOutputUnitStop(audioUnit_);
            if (status != noErr && status != kAudioUnitErr_Uninitialized) {
                spdlog::warn("Unable to stop CoreAudio output cleanly (status {})", status);
            }
        }
        readPosition_.store(writePosition_.load());
    }

    void close() override {
        if (audioUnit_ == nullptr) {
            return;
        }
        stopAndClear();
        AudioUnitUninitialize(audioUnit_);
        AudioComponentInstanceDispose(audioUnit_);
        audioUnit_ = nullptr;
        device_ = kAudioObjectUnknown;
        outputRing_.clear();
    }

    bool write(std::span<const int16_t> samples) override {
        if (channels_ == 0 || samples.size() % channels_ != 0 || outputRing_.empty()) {
            return false;
        }
        const uint64_t read = readPosition_.load(std::memory_order_acquire);
        const uint64_t write = writePosition_.load(std::memory_order_relaxed);
        const size_t usedSamples = static_cast<size_t>(write - read);
        if (samples.size() > outputRing_.size() - usedSamples) {
            spdlog::error("CoreAudio application queue is full (queued={}, requested={}, capacity={})", usedSamples,
                          samples.size(), outputRing_.size());
            return false;
        }
        const size_t start = static_cast<size_t>(write % outputRing_.size());
        const size_t first = std::min(samples.size(), outputRing_.size() - start);
        std::copy_n(samples.begin(), first, outputRing_.begin() + static_cast<std::ptrdiff_t>(start));
        std::copy(samples.begin() + static_cast<std::ptrdiff_t>(first), samples.end(), outputRing_.begin());
        writePosition_.store(write + samples.size(), std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t queuedFrames() const override {
        if (channels_ == 0) {
            return 0;
        }
        const uint64_t samples =
            writePosition_.load(std::memory_order_acquire) - readPosition_.load(std::memory_order_acquire);
        return static_cast<size_t>(samples / channels_) + (running_.load() ? pipelineLatencyFrames_ : 0);
    }
    [[nodiscard]] size_t pipelineLatencyFrames() const override { return pipelineLatencyFrames_; }
    [[nodiscard]] uint64_t underruns() const override { return underruns_.load(); }

  private:
    static OSStatus renderCallback(void *context, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32,
                                   UInt32 frames, AudioBufferList *buffers) {
        return static_cast<CoreAudioOutput *>(context)->render(frames, buffers);
    }

    OSStatus render(UInt32 frames, AudioBufferList *buffers) {
        if (buffers == nullptr || buffers->mNumberBuffers == 0 || buffers->mBuffers[0].mData == nullptr ||
            channels_ == 0) {
            return noErr;
        }
        auto *destination = static_cast<int16_t *>(buffers->mBuffers[0].mData);
        const size_t destinationSamples = std::min<size_t>(static_cast<size_t>(frames) * channels_,
                                                           buffers->mBuffers[0].mDataByteSize / sizeof(int16_t));
        const uint64_t read = readPosition_.load(std::memory_order_relaxed);
        const uint64_t write = writePosition_.load(std::memory_order_acquire);
        const size_t available = static_cast<size_t>(write - read);
        const size_t copied = std::min(destinationSamples, available);
        const size_t start = static_cast<size_t>(read % outputRing_.size());
        const size_t first = std::min(copied, outputRing_.size() - start);
        std::copy_n(outputRing_.begin() + static_cast<std::ptrdiff_t>(start), first, destination);
        std::copy_n(outputRing_.begin(), copied - first, destination + static_cast<std::ptrdiff_t>(first));
        std::fill(destination + static_cast<std::ptrdiff_t>(copied),
                  destination + static_cast<std::ptrdiff_t>(destinationSamples), 0);
        readPosition_.store(read + copied, std::memory_order_release);
        for (UInt32 index = 1; index < buffers->mNumberBuffers; ++index) {
            if (buffers->mBuffers[index].mData != nullptr) {
                std::memset(buffers->mBuffers[index].mData, 0, buffers->mBuffers[index].mDataByteSize);
            }
        }
        if (copied < destinationSamples && running_.load()) {
            underruns_.fetch_add(1);
        }
        return noErr;
    }

    bool check(OSStatus status, const char *operation) const {
        if (status == noErr) {
            return true;
        }
        spdlog::error("Unable to {} for CoreAudio output (status {})", operation, status);
        return false;
    }

    void requestDevicePeriod() {
        AudioObjectPropertyAddress address{kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        UInt32 requested = NATIVE_AUDIO_PERIOD_FRAMES;
        Boolean settable = false;
        if (AudioObjectIsPropertySettable(device_, &address, &settable) == noErr && settable) {
            static_cast<void>(AudioObjectSetPropertyData(device_, &address, 0, nullptr, sizeof(requested), &requested));
        }
        UInt32 size = sizeof(devicePeriodFrames_);
        if (!readProperty(device_, address, &devicePeriodFrames_, size)) {
            devicePeriodFrames_ = requested;
        }
    }

    size_t readPipelineLatency() const {
        UInt32 latency = 0;
        UInt32 safetyOffset = 0;
        UInt32 size = sizeof(UInt32);
        AudioObjectPropertyAddress latencyAddress{kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput,
                                                  kAudioObjectPropertyElementMain};
        readProperty(device_, latencyAddress, &latency, size);
        size = sizeof(UInt32);
        AudioObjectPropertyAddress safetyAddress{kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput,
                                                 kAudioObjectPropertyElementMain};
        readProperty(device_, safetyAddress, &safetyOffset, size);
        return static_cast<size_t>(latency) + safetyOffset + devicePeriodFrames_;
    }

    AudioUnit audioUnit_{nullptr};
    AudioDeviceID device_{kAudioObjectUnknown};
    UInt32 devicePeriodFrames_{NATIVE_AUDIO_PERIOD_FRAMES};
    size_t pipelineLatencyFrames_{0};
    uint32_t sampleRate_{0};
    uint16_t channels_{0};
    std::vector<int16_t> outputRing_;
    std::atomic<uint64_t> readPosition_{0};
    std::atomic<uint64_t> writePosition_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<bool> running_{false};
};

} // namespace

std::unique_ptr<AudioOutput> createAudioOutput() { return std::make_unique<CoreAudioOutput>(); }

void listAudioOutputDevices(std::ostream &output) {
    const auto devices = enumerateDevices();
    output << "Available CoreAudio devices:\n";
    output << "Number of audio devices: " << devices.size() << '\n';
    for (size_t index = 0; index < devices.size(); ++index) {
        output << "  " << devices[index].name;
        if (index == 0) {
            output << " (default)";
        }
        output << '\n';
    }
}

} // namespace creatures::audio
