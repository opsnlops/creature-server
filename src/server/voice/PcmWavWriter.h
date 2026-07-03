#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "util/Result.h"

namespace creatures::voice {

/// Wrap raw mono 16-bit PCM in a canonical 17-channel S16 LE WAV file, with
/// the source data placed on the given audioChannel (1-based) and silence on
/// every other lane. Matches the wire format the creature controller / show
/// player expects (48 kHz mono input, 17-channel output WAV).
///
/// This replaces the older `AudioConverter::convertMp3ToWav` ffmpeg pipeline
/// — now that ElevenLabs returns `pcm_48000` directly we can skip the
/// MP3 decode round-trip and write the WAV header in-process (issue #12).
///
/// @param pcmData     raw S16 LE mono PCM samples (typically straight from
///                    ElevenLabs `pcm_48000`)
/// @param wavPath     output file path
/// @param audioChannel 1-based target channel in [1, 17]
/// @param sampleRate  written into the WAV header; pass the source's actual
///                    rate (48000 for `pcm_48000`)
/// @return total bytes written (header + data) on success
Result<std::size_t> writePcmToMultichannelWav(const std::vector<uint8_t> &pcmData, const std::filesystem::path &wavPath,
                                              uint16_t audioChannel, uint32_t sampleRate);

/// Wrap raw mono S16 LE PCM in a canonical 44-byte mono WAV header, in memory.
/// The one shared implementation of the helper previously duplicated in
/// DialogPreviewController and JobWorker (issue #11).
std::vector<uint8_t> wrapMonoPcmAsWav(const std::vector<uint8_t> &pcm, uint32_t sampleRate);

} // namespace creatures::voice
