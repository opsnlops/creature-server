#include "AudioOutput.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>
#include <spdlog/spdlog.h>

namespace creatures::audio {
namespace {

struct AlsaDevice {
    std::string name;
    std::string description;
};

std::string singleLineDescription(const char *description) {
    if (description == nullptr) {
        return {};
    }
    std::string result(description);
    std::replace(result.begin(), result.end(), '\n', ' ');
    return result;
}

std::vector<AlsaDevice> enumerateDevices() {
    std::vector<AlsaDevice> devices{{"default", "Default ALSA output"}};
    std::unordered_set<std::string> seen{"default"};
    void **hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0 || hints == nullptr) {
        return devices;
    }
    for (void **hint = hints; *hint != nullptr; ++hint) {
        char *name = snd_device_name_get_hint(*hint, "NAME");
        char *description = snd_device_name_get_hint(*hint, "DESC");
        char *ioId = snd_device_name_get_hint(*hint, "IOID");
        const bool isOutput = ioId == nullptr || std::strcmp(ioId, "Output") == 0;
        if (name != nullptr && isOutput && std::strcmp(name, "null") != 0 && seen.emplace(name).second) {
            devices.push_back({name, singleLineDescription(description)});
        }
        std::free(name);
        std::free(description);
        std::free(ioId);
    }
    snd_device_name_free_hint(hints);
    return devices;
}

class AlsaAudioOutput final : public AudioOutput {
  public:
    ~AlsaAudioOutput() override { close(); }

    bool open(const NativeAudioConfig &config) override {
        close();
        channels_ = config.channels;
        sampleRate_ = config.sampleRate;
        if (channels_ == 0 || sampleRate_ == 0) {
            spdlog::error("Invalid ALSA output format: {} Hz, {} channels", sampleRate_, channels_);
            return false;
        }

        std::string deviceName;
        if (config.deviceName.has_value()) {
            deviceName = *config.deviceName;
        } else {
            deviceName = "default";
        }

        int result = snd_pcm_open(&pcm_, deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
        if (result < 0) {
            spdlog::error("Unable to open configured ALSA output '{}': {}", deviceName, snd_strerror(result));
            pcm_ = nullptr;
            return false;
        }

        snd_pcm_hw_params_t *hardwareParameters = nullptr;
        snd_pcm_hw_params_alloca(&hardwareParameters);
        if (!check(snd_pcm_hw_params_any(pcm_, hardwareParameters), "initialize hardware parameters") ||
            !check(snd_pcm_hw_params_set_access(pcm_, hardwareParameters, SND_PCM_ACCESS_RW_INTERLEAVED),
                   "set interleaved access") ||
            !check(snd_pcm_hw_params_set_format(pcm_, hardwareParameters, SND_PCM_FORMAT_S16),
                   "set signed 16-bit format") ||
            !check(snd_pcm_hw_params_set_channels(pcm_, hardwareParameters, channels_), "set output channels")) {
            close();
            return false;
        }

        unsigned int negotiatedRate = sampleRate_;
        int direction = 0;
        if (!check(snd_pcm_hw_params_set_rate_near(pcm_, hardwareParameters, &negotiatedRate, &direction),
                   "set sample rate") ||
            negotiatedRate != sampleRate_) {
            spdlog::error("ALSA output '{}' cannot provide required {} Hz sample rate (received {})", deviceName,
                          sampleRate_, negotiatedRate);
            close();
            return false;
        }

        snd_pcm_uframes_t requestedPeriod = NATIVE_AUDIO_PERIOD_FRAMES;
        snd_pcm_uframes_t requestedBuffer = sampleRate_ / 5; // 200 ms, bounded by hardware negotiation.
        if (!check(snd_pcm_hw_params_set_period_size_near(pcm_, hardwareParameters, &requestedPeriod, &direction),
                   "set period size") ||
            !check(snd_pcm_hw_params_set_buffer_size_near(pcm_, hardwareParameters, &requestedBuffer),
                   "set buffer size") ||
            !check(snd_pcm_hw_params(pcm_, hardwareParameters), "apply hardware parameters")) {
            close();
            return false;
        }
        snd_pcm_hw_params_get_period_size(hardwareParameters, &periodFrames_, &direction);
        snd_pcm_hw_params_get_buffer_size(hardwareParameters, &bufferFrames_);

        snd_pcm_sw_params_t *softwareParameters = nullptr;
        snd_pcm_sw_params_alloca(&softwareParameters);
        snd_pcm_uframes_t boundary = 0;
        if (!check(snd_pcm_sw_params_current(pcm_, softwareParameters), "read software parameters") ||
            !check(snd_pcm_sw_params_get_boundary(softwareParameters, &boundary), "read PCM boundary") ||
            !check(snd_pcm_sw_params_set_start_threshold(pcm_, softwareParameters, boundary),
                   "disable automatic start") ||
            !check(snd_pcm_sw_params_set_avail_min(pcm_, softwareParameters, periodFrames_), "set wakeup period") ||
            !check(snd_pcm_sw_params(pcm_, softwareParameters), "apply software parameters") ||
            !check(snd_pcm_prepare(pcm_), "prepare output")) {
            close();
            return false;
        }

        spdlog::info("ALSA output ready: device '{}', {} Hz, {} channels, period={} frames, buffer={} frames",
                     deviceName, sampleRate_, channels_, periodFrames_, bufferFrames_);
        return true;
    }

    bool start() override {
        if (pcm_ == nullptr) {
            return false;
        }
        const int result = snd_pcm_start(pcm_);
        if (result < 0) {
            spdlog::error("Unable to start ALSA output: {}", snd_strerror(result));
            return false;
        }
        started_ = true;
        return true;
    }

    void stopAndClear() override {
        if (pcm_ == nullptr) {
            return;
        }
        started_ = false;
        snd_pcm_drop(pcm_);
        const int result = snd_pcm_prepare(pcm_);
        if (result < 0) {
            spdlog::warn("Unable to prepare ALSA output after clearing it: {}", snd_strerror(result));
        }
    }

    void close() override {
        if (pcm_ == nullptr) {
            return;
        }
        started_ = false;
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }

    bool write(std::span<const int16_t> samples) override {
        if (pcm_ == nullptr || channels_ == 0 || samples.size() % channels_ != 0) {
            return false;
        }
        const size_t totalFrames = samples.size() / channels_;
        size_t frameOffset = 0;
        int waitsRemaining = 20;
        bool recovered = false;
        while (frameOffset < totalFrames) {
            const snd_pcm_sframes_t written = snd_pcm_writei(pcm_, samples.data() + frameOffset * channels_,
                                                             static_cast<snd_pcm_uframes_t>(totalFrames - frameOffset));
            if (written > 0) {
                frameOffset += static_cast<size_t>(written);
                continue;
            }
            if (written == -EAGAIN && waitsRemaining-- > 0) {
                snd_pcm_wait(pcm_, NATIVE_AUDIO_WAIT_MS);
                continue;
            }
            if (written == -EPIPE || written == -ESTRPIPE) {
                underruns_.fetch_add(1);
                const int recoverResult = snd_pcm_recover(pcm_, static_cast<int>(written), 1);
                if (recoverResult < 0) {
                    spdlog::error("Unable to recover ALSA output: {}", snd_strerror(recoverResult));
                    return false;
                }
                recovered = true;
                continue;
            }
            ++consecutiveWriteFailures_;
            if (consecutiveWriteFailures_ == 1 || consecutiveWriteFailures_ % 200 == 0) {
                spdlog::error("Unable to write ALSA audio: {} ({} consecutive failures)",
                              snd_strerror(static_cast<int>(written)), consecutiveWriteFailures_);
            }
            return false;
        }
        if (consecutiveWriteFailures_ > 0) {
            spdlog::info("ALSA audio writes recovered after {} failures", consecutiveWriteFailures_);
            consecutiveWriteFailures_ = 0;
        }
        if (recovered && started_ && snd_pcm_start(pcm_) < 0) {
            spdlog::error("Unable to restart ALSA output after underrun");
            return false;
        }
        return true;
    }

    [[nodiscard]] size_t queuedFrames() const override {
        if (pcm_ == nullptr) {
            return 0;
        }
        snd_pcm_sframes_t delay = 0;
        return snd_pcm_delay(pcm_, &delay) < 0 || delay <= 0 ? 0 : static_cast<size_t>(delay);
    }
    [[nodiscard]] size_t pipelineLatencyFrames() const override { return 0; }
    [[nodiscard]] uint64_t underruns() const override { return underruns_.load(); }

  private:
    bool check(int result, const char *operation) const {
        if (result >= 0) {
            return true;
        }
        spdlog::error("Unable to {} for ALSA output: {}", operation, snd_strerror(result));
        return false;
    }

    snd_pcm_t *pcm_{nullptr};
    snd_pcm_uframes_t periodFrames_{0};
    snd_pcm_uframes_t bufferFrames_{0};
    uint32_t sampleRate_{0};
    uint16_t channels_{0};
    bool started_{false};
    std::atomic<uint64_t> underruns_{0};
    uint64_t consecutiveWriteFailures_{0};
};

} // namespace

std::unique_ptr<AudioOutput> createAudioOutput() { return std::make_unique<AlsaAudioOutput>(); }

void listAudioOutputDevices(std::ostream &output) {
    const auto devices = enumerateDevices();
    output << "Available ALSA audio devices:\n";
    output << "Number of audio devices: " << devices.size() << '\n';
    for (const auto &device : devices) {
        output << "  " << device.name;
        if (!device.description.empty()) {
            output << " — " << device.description;
        }
        output << '\n';
    }
}

} // namespace creatures::audio
