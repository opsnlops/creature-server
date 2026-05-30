#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "DialogPipeline.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/// Map from ElevenLabs voice_id to that creature's audio_channel.
///
/// `audio_channel` is **1-based** (matches the value stored on each creature
/// in MongoDB; valid range is 1..17 where 1..16 are creature lanes and 17 is
/// the BGM lane). The DialogWav writer subtracts 1 internally to get the
/// 0-based interleave lane.
using VoiceChannelMap = std::unordered_map<std::string, uint16_t>;

/// Write an assembled dialog scene to disk as the 48 kHz / S16 / 17-channel
/// interleaved WAV that AudioStreamBuffer::loadFromWavFile and the external
/// creature-controller both depend on.
///
/// Each creature's mono PCM is placed in its `audio_channel` lane; every
/// other lane is zero. The output file is a standard PCM WAV — 44-byte
/// canonical header, then the interleaved samples.
///
/// Validation (returns InvalidData on failure):
/// - `assembled.sampleRate` must be 48000 (the only rate the streaming audio
///   path will accept).
/// - Every voice in `assembled.perCreature` must have an entry in
///   `voiceToChannel`. Voices in the map without a matching perCreature entry
///   are fine (they just don't appear in the output).
/// - Each mapped channel must be in [1, 17] and distinct from every other
///   mapped channel (two creatures clobbering the same lane = silent data
///   loss; fail at submit time instead).
/// - `assembled.totalSamples` * 17 * 2 bytes must fit in a uint32_t — the WAV
///   header's size fields are 32-bit. ~4.2 GiB total, or about an hour of
///   17-channel audio at 48 kHz. Realistic dialog scenes are well under this.
///
/// Builds the full interleaved buffer in memory before writing — the file is
/// only loaded back via loadFromWavFile, which itself reads the whole thing
/// into memory, so there's no streaming win from doing it incrementally.
///
/// On success, the file at `outPath` is suitable for direct hand-off to
/// AudioStreamBuffer::loadFromWavFile.
Result<void> writeDialogWav(const DialogAssembled &assembled, const VoiceChannelMap &voiceToChannel,
                            const std::filesystem::path &outPath, std::shared_ptr<OperationSpan> parentSpan = nullptr);

} // namespace creatures::voice
