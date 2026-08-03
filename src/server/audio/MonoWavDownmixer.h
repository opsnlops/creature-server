#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "util/Result.h"

namespace creatures::audio {

struct DownmixConfig {
    float dialogGainDb{0.0F};
    float bgmGainDb{0.0F};
    float limiterCeilingDb{0.0F};
};

/**
 * A WAV file rendered down to a single mono channel
 */
struct MonoWav {
    std::vector<int16_t> samples;
    int sampleRate = 0;
};

/**
 * Bounded-memory reader for signed 16-bit PCM WAV files.
 *
 * The reader keeps only the caller-requested chunk in memory. It supports raw
 * interleaved reads, mono export, and bounded-memory local-output mixes, so
 * a long 17-channel file does not need to fit in the Raspberry Pi's RAM.
 */
class MonoWavStream {
  public:
    static Result<std::shared_ptr<MonoWavStream>> open(const std::string &filePath, DownmixConfig downmixConfig = {});

    /**
     * Read and downmix up to output.size() sample frames.
     *
     * @return number of mono samples written; zero means end of file
     */
    Result<size_t> readMonoFrames(std::span<int16_t> output);

    /**
     * Render travel audio as interleaved stereo.
     *
     * For the workshop's 17-channel format, dialog lanes 1 and 2 route to
     * output channels 1 and 2 respectively. BGM lane 17 is added equally to
     * both outputs. Mono WAVs are duplicated; other multichannel WAVs preserve
     * their first two lanes.
     */
    Result<size_t> readTravelStereoFrames(std::span<int16_t> interleavedStereoOutput);

    /**
     * Render WAV audio to the configured native output channel count.
     *
     * A 17-channel workshop WAV routes dialog lanes to their matching output
     * lanes and adds BGM lane 17 equally to every dialog output. Mono input is
     * duplicated; ordinary multichannel input preserves matching lanes.
     */
    Result<size_t> readLocalOutputFrames(std::span<int16_t> interleavedOutput, uint16_t outputChannels);

    /** Read interleaved PCM frames without downmixing. */
    Result<size_t> readInterleavedFrames(std::span<int16_t> output);

    [[nodiscard]] int sampleRate() const { return sampleRate_; }
    [[nodiscard]] uint16_t channels() const { return channels_; }
    [[nodiscard]] uint64_t totalFrames() const { return totalFrames_; }

  private:
    MonoWavStream() = default;

    std::ifstream file_;
    uint16_t channels_{0};
    uint16_t blockAlign_{0};
    int sampleRate_{0};
    uint64_t totalFrames_{0};
    uint64_t dataBytesRemaining_{0};
    std::vector<uint8_t> sourceBuffer_;
    std::vector<int16_t> interleavedBuffer_;
    float dialogGainLinear_{1.0F};
    float bgmGainLinear_{1.0F};
    float limiterCeilingLinear_{1.0F};
};

/**
 * Downmix interleaved signed 16-bit PCM to mono
 *
 * Every channel is summed into a 32-bit accumulator and clamped to the int16
 * range. In practice only a few of the 17 animation channels carry signal at
 * any moment, so a straight sum keeps dialog at full loudness; the clamp
 * prevents wraparound when several loud channels stack.
 *
 * @param interleaved interleaved source samples (frames * channels of them)
 * @param frames number of sample frames
 * @param channels number of interleaved channels (must be > 0)
 * @param out destination buffer with room for `frames` samples
 */
void downmixToMono(const int16_t *interleaved, size_t frames, int channels, int16_t *out);

/**
 * Load a WAV file and render it to mono
 *
 * Accepts any channel count (the 17-channel animation tracks as well as plain
 * mono or stereo clips) but requires signed 16-bit samples, which is what every
 * sound in the workshop is stored as.
 *
 * @param filePath path to the WAV file on disk
 * @param maximumFrames optional maximum number of sample frames to accept;
 * rejects the file from its header before allocating the destination buffer
 * @return the mono samples and the file's sample rate, or an error
 */
Result<MonoWav> loadWavAsMono(const std::string &filePath, std::optional<std::uint64_t> maximumFrames = std::nullopt);

} // namespace creatures::audio
