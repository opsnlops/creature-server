
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "server/audio/OggOpusWriter.h" // for kShareableSampleRate (the shared 48kHz pipeline rate)
#include "util/Result.h"

namespace creatures::audio {

/// User comments to embed as ID3v2 TXXX (user-defined text) frames, as (key, value)
/// pairs. Used to mirror dialog provenance (TITLE, SOURCE_SCRIPT_ID, DESCRIPTION) into a
/// shared .mp3, the same way OggComments mirrors it into the Ogg's OpusTags (#47, #57).
using Id3Comments = std::vector<std::pair<std::string, std::string>>;

/// Bitrate for the shareable MP3 rendition. 128 kbps CBR mono matches the Ogg endpoint's
/// "transparent even on phone speakers" target and keeps a multi-hundred-MB 17-channel
/// dialog render down to something Slack/AVFoundation/browsers stream instantly (#57).
inline constexpr int kShareableMp3Bitrate = 128000;

/**
 * Encode mono S16 PCM at 48 kHz into a complete MP3 (MPEG-1 Audio Layer III) file in
 * memory, 128 kbps CBR mono by default.
 *
 * Input is expected straight from MonoWavDownmixer::loadWavAsMono. Rejects empty input
 * and any sample rate other than 48 kHz with InvalidData — the whole audio path is 48 kHz
 * by design, so a non-48k file in the sound stores is a pipeline smell to surface, not
 * silently resample (same contract as encodeMonoToOggOpus).
 *
 * If @p comments is non-empty, a minimal ID3v2.4.0 tag carrying one TXXX frame per entry is
 * prepended to the stream, so a shared/downloaded MP3 also carries its dialog provenance.
 *
 * @param samples mono S16 PCM
 * @param sampleRate must be kShareableSampleRate
 * @param bitrate target CBR bitrate in bits/second
 * @param comments optional ID3v2 TXXX (key, value) frames
 * @return the bytes of a playable .mp3 file, or an error
 */
Result<std::vector<uint8_t>> encodeMonoToMp3(const std::vector<int16_t> &samples, int sampleRate,
                                             int bitrate = kShareableMp3Bitrate, const Id3Comments &comments = {});

} // namespace creatures::audio
