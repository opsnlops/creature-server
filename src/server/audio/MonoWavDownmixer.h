#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/Result.h"

namespace creatures::audio {

/**
 * A WAV file rendered down to a single mono channel
 */
struct MonoWav {
    std::vector<int16_t> samples;
    int sampleRate = 0;
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
 * @return the mono samples and the file's sample rate, or an error
 */
Result<MonoWav> loadWavAsMono(const std::string &filePath);

} // namespace creatures::audio
