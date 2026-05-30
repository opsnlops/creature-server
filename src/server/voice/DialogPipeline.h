#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "DialogClient.h"
#include "TextToViseme.h"
#include "util/Result.h"

namespace creatures::voice {

/// Tuning constants for the dialog assembly pipeline.
///
/// These came directly from the show.py prototype, which was validated by ear
/// on Mango+Beaky scenes. Don't tweak without re-listening — the slicing math
/// depends on these being internally consistent (e.g. PAD_OUT > SEAM_FADE to
/// avoid the fade eating audible audio).
namespace dialog_pipeline {

/// Keep this much natural onset before each turn's first word, in seconds.
constexpr double PAD_IN_SECS = 0.08;

/// Keep this much natural release after each turn's last word, in seconds.
constexpr double PAD_OUT_SECS = 0.22;

/// Controlled silence inserted between turns on the tightened timeline (vs. the
/// ~0.5–1.0s of dead air the raw v3 mixdown leaves).
constexpr double INTER_TURN_GAP_SECS = 0.12;

/// Linear fade-in/fade-out at the seam of each slice to kill click artifacts
/// from cutting samples mid-waveform. ~8 ms is the show.py default.
constexpr double SEAM_FADE_SECS = 0.008;

/// Silence inserted between chunks (when the scene was split across multiple
/// dialog calls because the per-call ~2000-char cap was exceeded). Slightly
/// longer than the inter-turn gap because chunks lose cross-speaker reactivity
/// at the seam, so a clearer pause reads as natural.
constexpr double INTER_CHUNK_GAP_SECS = 0.30;

/// Hard cap on per-chunk character count. ElevenLabs' text-to-dialogue endpoint
/// rejects requests over ~2000 chars; we sit a bit under to leave headroom for
/// audio tags expanding the effective payload.
constexpr std::size_t MAX_CHARS_PER_CHUNK = 1800;

/// Default sample rate. We standardize on the server's native 48 kHz, which
/// matches the pcm_48000 output format used end-to-end.
constexpr uint32_t DEFAULT_SAMPLE_RATE = 48000;

} // namespace dialog_pipeline

/// One creature's slice of an assembled dialog: mono PCM on the tightened
/// timeline, plus per-character mouth timing on that same timeline.
///
/// The PCM is full-length (== DialogAssembled::totalSamples) with zeros in the
/// time slots where this creature isn't speaking. That keeps the per-creature
/// buffers trivially mixable into the 17-channel WAV downstream — each
/// creature's samples go into its `audio_channel` lane and the silence in the
/// other lanes is the literal silence already in this buffer.
struct DialogPerCreature {
    /// The ElevenLabs voice_id this creature was generated as.
    std::string voiceId;

    /// Mono 16-bit signed samples at DialogAssembled::sampleRate.
    std::vector<int16_t> pcm;

    /// Per-character timing on the tightened timeline (startTimeMs from start
    /// of the assembled audio, not from the start of any individual turn).
    /// Feeds straight into TextToViseme::charTimingsToMouthCues().
    std::vector<TextToViseme::CharTiming> mouth;
};

/// Result of assembling one or more chunks: per-creature mono PCM + mouth
/// timing, all on a single shared tightened timeline.
struct DialogAssembled {
    /// One entry per unique voice that spoke. PCM length is totalSamples for
    /// every entry; voices that didn't speak in a given turn have zeros there.
    std::vector<DialogPerCreature> perCreature;

    /// Length of the tightened timeline in samples (== pcm.size() for every
    /// perCreature entry).
    std::size_t totalSamples = 0;

    /// Sample rate of every PCM buffer here.
    uint32_t sampleRate = dialog_pipeline::DEFAULT_SAMPLE_RATE;
};

/// Split an ordered list of turns into chunks each <= maxChars total.
///
/// Splitting only happens at turn boundaries — never mid-turn — because the
/// ElevenLabs dialog endpoint generates a whole submission jointly and we lose
/// cross-speaker reactivity at any chunk seam. Pick scene breaks for long
/// scenes; this function just enforces the cap.
///
/// Fails with InvalidData if a single turn's text alone exceeds maxChars (the
/// API would reject that chunk anyway, so it's a fatal authoring error).
Result<std::vector<std::vector<DialogInput>>> chunkTurns(const std::vector<DialogInput> &turns,
                                                         std::size_t maxChars = dialog_pipeline::MAX_CHARS_PER_CHUNK);

/// Assemble one chunk's audio + timing into per-creature outputs.
///
/// Inputs:
/// - `turns`: the chunk's ordered turns, exactly as sent to generateDialog().
/// - `dialog`: the response from generateDialog() — provides the mixed audio.
/// - `alignment`: the response from forcedAlignment() on `dialog.audioData`
///   with the tag-stripped transcript (built by joining stripTags(turn.text)
///   for each turn with single spaces).
/// - `sampleRate`: must match the dialog audio (pcm_48000 → 48000).
///
/// Algorithm (port of show.py steps 3+4, validated by ear on the prototype):
/// 1. Walk turns in order. For each, consume len(stripped.split()) words from
///    `alignment.words` to bracket the turn's audio. Consume len(stripped)
///    chars from `alignment.characters` for mouth timing. Advance the char
///    cursor by 1 to skip the inter-turn space separator.
/// 2. Slice the mixdown per turn: clamp the cut to the midpoint of the gap
///    with each neighbour; keep PAD_IN onset + PAD_OUT release; apply 8ms
///    seam fades so the cut doesn't click.
/// 3. Place each turn's slice back-to-back on a tightened timeline with
///    INTER_TURN_GAP_SECS of silence between turns. Shift each turn's char
///    timings by (write_position - original_position) / sampleRate so the
///    mouth tracks the audio after tightening.
///
/// Returns one DialogPerCreature per unique voice in the chunk. Voices that
/// didn't speak in any turn of this chunk don't get an entry — callers needing
/// the full scene's voice set should use concatChunks() or pad themselves.
Result<DialogAssembled> assembleChunk(const std::vector<DialogInput> &turns, const DialogResult &dialog,
                                      const ForcedAlignmentResult &alignment,
                                      uint32_t sampleRate = dialog_pipeline::DEFAULT_SAMPLE_RATE);

/// Concatenate multiple per-chunk DialogAssembled results into one whole-scene
/// result, in order.
///
/// All chunks must share the same sampleRate. The output's voice set is the
/// union of the inputs'; for any chunk where a voice doesn't appear, that
/// voice's lane gets zero-padded for the chunk's length. A small inter-chunk
/// gap (INTER_CHUNK_GAP_SECS) of silence is inserted between chunks.
///
/// Mouth timings carry through, offset by the accumulated prior samples + gaps.
Result<DialogAssembled> concatChunks(const std::vector<DialogAssembled> &chunks);

} // namespace creatures::voice
