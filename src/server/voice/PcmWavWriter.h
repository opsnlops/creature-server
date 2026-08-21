#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "server/voice/IxmlWriter.h"
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

/// Result of stitching session parts into one WAV (issue #150).
struct StitchedWavInfo {
    uint64_t totalDurationMs{0};
    std::vector<uint64_t> partDurationsMs; // parallel to the input paths
};

/// Concatenate the PCM of several same-format multichannel WAVs into one file,
/// appending an iXML chunk built from `provenance` (issue #150). This is how a
/// streaming ad-hoc session's s1..sN.wav become a single exchange WAV: parts
/// are identical 17-channel/48k/S16 files and playback is gapless, so plain
/// data-chunk concatenation reproduces exactly what was heard.
///
/// Every input must match the first part's fmt (PCM, channel count, sample
/// rate, 16-bit); a mismatch is InvalidData. Data is streamed, not slurped —
/// a long exchange never has to fit in memory.
Result<StitchedWavInfo> stitchMultichannelWavs(const std::vector<std::filesystem::path> &parts,
                                               const std::filesystem::path &outPath, const WavProvenance &provenance);

/// Wrap raw mono S16 LE PCM in a canonical 44-byte mono WAV header, in memory.
/// The one shared implementation of the helper previously duplicated in
/// DialogPreviewController and JobWorker (issue #11).
///
/// If `provenance` is non-null and non-empty, an iXML chunk describing the
/// scene's script is appended after the `data` chunk (#50). Readers that walk
/// chunks and honor the `data` size ignore it, so playback is unaffected.
std::vector<uint8_t> wrapMonoPcmAsWav(const std::vector<uint8_t> &pcm, uint32_t sampleRate,
                                      const WavProvenance *provenance = nullptr);

} // namespace creatures::voice
