
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "util/Result.h"

namespace creatures::audio {

/// User comments to embed in the Ogg's OpusTags header, as (key, value) pairs
/// written `KEY=VALUE` per the Vorbis-comment convention. Used to mirror dialog
/// provenance (TITLE, DESCRIPTION, SOURCE_SCRIPT_ID) into a shared .ogg (#47).
using OggComments = std::vector<std::pair<std::string, std::string>>;

/// Bitrate for shareable sound exports. 96 kbps mono Opus is transparent for our
/// content while turning a multi-megabyte WAV into something chat-app sized.
inline constexpr int kShareableOpusBitrate = 96000;

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
                                                 int bitrate = kShareableOpusBitrate, const OggComments &comments = {});

} // namespace creatures::audio
