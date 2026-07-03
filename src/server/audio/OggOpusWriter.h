
#pragma once

#include <cstdint>
#include <vector>

#include "util/Result.h"

namespace creatures::audio {

/// Bitrate for shareable sound exports. 128 kbps mono Opus stays transparent even on
/// music beds (96 kbps had audible artifacting) while turning a multi-megabyte WAV into
/// something chat-app sized.
inline constexpr int kShareableOpusBitrate = 128000;

/// The whole audio pipeline runs at 48 kHz by design (Opus's native rate), and the
/// muxer's pre-skip/granule bookkeeping relies on it: Ogg Opus granule positions are
/// defined in 48 kHz units (RFC 7845 §4).
inline constexpr int kShareableSampleRate = 48000;

/**
 * Encode mono S16 PCM at 48 kHz into a complete Ogg/Opus (RFC 7845) file in memory.
 *
 * Input is expected straight from MonoWavDownmixer::loadWavAsMono. Rejects empty
 * input and any sample rate other than 48 kHz with InvalidData — a non-48k file in
 * the sound stores is a pipeline smell to surface, not silently resample.
 *
 * @param samples mono S16 PCM
 * @param sampleRate must be kShareableSampleRate
 * @param bitrate target bitrate in bits/second
 * @return the bytes of a playable .ogg file, or an error
 */
Result<std::vector<uint8_t>> encodeMonoToOggOpus(const std::vector<int16_t> &samples, int sampleRate,
                                                 int bitrate = kShareableOpusBitrate);

} // namespace creatures::audio
