#pragma once

#include <cstdint>
#include <filesystem>

#include "util/Result.h"

namespace creatures::voice {

/// Metadata pulled out of a WAV file's fmt + data chunks. Sample data is NOT
/// loaded — use the offset + size to seek and read selectively.
struct WavInfo {
    uint16_t audioFormat;     // PCM = 1
    uint16_t numChannels;     // 1 for mono, 17 for our show WAVs
    uint32_t sampleRate;      // Hz, typically 48000
    uint16_t bitsPerSample;   // typically 16 (S16)
    std::uint64_t dataOffset; // byte offset where the data chunk's samples begin
    std::uint64_t dataBytes;  // size of the data chunk's sample bytes
};

/// Read a canonical PCM WAV file's header chunks (RIFF/WAVE → fmt → ... → data),
/// skipping any unknown intermediate chunks. Validates that the file is RIFF/WAVE,
/// PCM-format, and that the fmt chunk parses cleanly. Returns the captured
/// metadata, or an InvalidData error if anything is malformed.
///
/// This is the in-process replacement for the previous ffmpeg-based
/// `AudioConverter::getChannelCount` (issue #12). We only ever consume WAV
/// files we wrote ourselves, so PCM-only is fine.
Result<WavInfo> readWavInfo(const std::filesystem::path &path);

/// Read just the channel count from `path`. Convenience wrapper around
/// `readWavInfo` for the common "how many channels does this WAV have?" check.
Result<int> readWavChannelCount(const std::filesystem::path &path);

/// Extract one channel of a multi-channel S16 PCM WAV into a mono S16 PCM
/// WAV at `outputPath`. `channelIndex` is 1-based and must be in
/// `[1, source.numChannels]`. Same sample rate as the source.
///
/// In-process replacement for `AudioConverter::extractChannelToMono`
/// (issue #12). Bits-per-sample must be 16; we'd error out on anything else
/// rather than silently down/up-mix.
Result<void> extractChannelToMonoWav(const std::filesystem::path &sourcePath, const std::filesystem::path &outputPath,
                                     int channelIndex);

} // namespace creatures::voice
