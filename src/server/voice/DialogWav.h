#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

#include "DialogPipeline.h"
#include "server/voice/IxmlWriter.h"
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

/// Return the show timeline length in 48 kHz samples. Dialog rendering follows
/// whichever accepted asset ends last: spoken dialog or background music.
std::size_t dialogPlaybackSampleCount(const DialogAssembled &assembled, std::span<const int16_t> backgroundMusic = {});

/// Write an assembled dialog scene to disk as the 48 kHz / S16 / 17-channel
/// interleaved WAV that AudioStreamBuffer::loadFromWavFile and the external
/// creature-controller both depend on.
///
/// Each creature's mono PCM is placed in its `audio_channel` lane; every
/// other lane is zero. The output lasts until the later of the dialog or
/// background music; creature lanes are silent during a longer music tail.
/// The output file is a standard PCM WAV — 44-byte canonical header, then the
/// interleaved samples.
///
/// Validation (returns InvalidData on failure):
/// - `assembled.sampleRate` must be 48000 (the only rate the streaming audio
///   path will accept).
/// - Every voice in `assembled.perCreature` must have an entry in
///   `voiceToChannel`. Voices in the map without a matching perCreature entry
///   are fine (they just don't appear in the output).
/// - Each mapped creature channel must be in [1, 16]; channel 17 is reserved
///   for `backgroundMusic`.
///   mapped channel (two creatures clobbering the same lane = silent data
///   loss; fail at submit time instead).
/// - `max(assembled.totalSamples, backgroundMusic.size())` * 17 * 2 bytes must
///   fit in a uint32_t — the WAV
///   header's size fields are 32-bit. ~4.2 GiB total, or about an hour of
///   17-channel audio at 48 kHz. Realistic dialog scenes are well under this.
///
/// Interleaving is written in bounded blocks so long music tails do not require
/// the full 17-channel WAV to exist in memory at once.
///
/// On success, the file at `outPath` is suitable for direct hand-off to
/// AudioStreamBuffer::loadFromWavFile.
///
/// If `provenance` is non-null and non-empty, an iXML chunk describing the
/// scene's script and channel layout is appended after the `data` chunk (issue
/// #47). Every server-side WAV reader walks chunks and honors the declared
/// `data` size, so the trailing chunk is invisible to playback and downmix.
Result<void> writeDialogWav(const DialogAssembled &assembled, const VoiceChannelMap &voiceToChannel,
                            const std::filesystem::path &outPath, std::shared_ptr<OperationSpan> parentSpan = nullptr,
                            const WavProvenance *provenance = nullptr, std::span<const int16_t> backgroundMusic = {});

} // namespace creatures::voice
