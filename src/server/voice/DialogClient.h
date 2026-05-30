#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/// One turn of a multi-character dialog submission.
struct DialogInput {
    std::string voiceId;
    std::string text; // may contain inline audio tags like [giggles]
};

/// One contiguous run of a single speaker in the mixed dialog audio.
///
/// On eleven_v3 the times in voice_segments[] are unreliable (the whole alignment is
/// crammed into a tiny window — confirmed empirically). The CHARACTER INDEX ranges,
/// however, are intact and contiguous. We carry the times for completeness/debugging
/// but real timing must come from a follow-up forced-alignment call.
struct DialogVoiceSegment {
    std::string voiceId;
    /// First character (inclusive) and one-past-last character (exclusive) into the
    /// concatenated character array — reliable on v3.
    std::size_t characterStartIndex = 0;
    std::size_t characterEndIndex = 0;
    /// Index into the original `inputs[]` array.
    std::size_t dialogInputIndex = 0;
    /// Reported times — DO NOT TRUST on eleven_v3; kept for diagnostics.
    double startTimeSeconds = 0.0;
    double endTimeSeconds = 0.0;
};

/// Result of a Text-to-Dialog request.
///
/// The audio is a single mixed-down stream (overlapping speech is physically
/// rendered in the waveform). Per-creature isolation requires slicing this mixdown
/// using turn boundaries — see ForcedAlignmentResult.
struct DialogResult {
    /// Raw audio data (typically interleaved-mono PCM at 48 kHz; see audioFormat).
    std::vector<uint8_t> audioData;

    /// Audio format string echoed back ("pcm_48000", etc.).
    std::string audioFormat;

    /// Concatenated characters in the spoken transcript (with any audio tags stripped
    /// by the model — we strip-and-rebuild ourselves for forced alignment, so this
    /// is mainly for debugging / index sanity checks).
    std::vector<std::string> alignmentCharacters;

    /// One entry per contiguous speaker run, in the order they appear in the audio.
    std::vector<DialogVoiceSegment> voiceSegments;

    /// Estimated audio duration in seconds (from audio byte count).
    double audioDurationSeconds = 0.0;

    /// Request ID from response header.
    std::string requestId;
};

/// One word from forced-alignment output.
struct ForcedAlignmentWord {
    std::string text;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
};

/// One character from forced-alignment output (includes spaces / separators).
struct ForcedAlignmentChar {
    std::string text;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
};

/// Result of a forced-alignment request — real per-character / per-word timing
/// for a given (audio, transcript) pair. This is what rescues timing for v3
/// dialog audio where the model's own timestamps are unreliable.
struct ForcedAlignmentResult {
    std::vector<ForcedAlignmentWord> words;
    std::vector<ForcedAlignmentChar> characters;
    /// Lower is better — empirically <0.1 on clean v3 dialog audio.
    double loss = 0.0;
};

/**
 * Client for the multi-character side of the ElevenLabs API.
 *
 * Wraps two endpoints used together to produce per-character dialog with
 * per-character timing:
 *
 * - **Text-to-Dialogue** (`/v1/text-to-dialogue/with-timestamps`, `eleven_v3`):
 *   joint generation of N voices into a single mixed-down audio stream, with
 *   speaker / character-index ranges. Returns reliable speaker→text mapping
 *   but the timestamps on v3 are broken.
 * - **Forced Alignment** (`/v1/forced-alignment`, multipart): real per-word /
 *   per-character timing for a given (audio, transcript). Used to rescue v3
 *   dialog audio whose own timestamps are unreliable.
 *
 * Stateless — every call is a fresh HTTPS request via libcurl.
 */
class DialogClient {
  public:
    DialogClient() = default;
    ~DialogClient() = default;

    // Non-copyable, non-movable — no state, but keep call sites honest about
    // ownership.
    DialogClient(const DialogClient &) = delete;
    DialogClient &operator=(const DialogClient &) = delete;
    DialogClient(DialogClient &&) = delete;
    DialogClient &operator=(DialogClient &&) = delete;

    /**
     * Generate multi-character dialog via the ElevenLabs Text-to-Dialogue API.
     *
     * Endpoint: POST /v1/text-to-dialogue/with-timestamps?output_format={format}
     * Model is fixed to eleven_v3 — turbo/multilingual are server-rejected for
     * this endpoint with HTTP 400 "does not support dialogue". This deliberately
     * bypasses the eleven_v3 blocklist applied to the ad-hoc single-character
     * path in StreamingTTSClient.
     *
     * Returns the single mixed-down audio and voice_segments. Per-creature
     * isolation is the caller's job (slice the mixdown by turn). The
     * voice_segments' character index ranges are reliable on v3; the times are
     * NOT — call forcedAlignment() with the tag-stripped transcript to get
     * real timing.
     *
     * @param apiKey ElevenLabs API key
     * @param inputs Ordered list of turns ({voice_id, text}); text may contain [tags]
     * @param outputFormat e.g. "pcm_48000" (Pro), "mp3_44100_192"
     * @param parentSpan Optional observability span
     */
    Result<DialogResult> generateDialog(const std::string &apiKey, const std::vector<DialogInput> &inputs,
                                        const std::string &outputFormat,
                                        std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Forced alignment of a transcript against an audio file.
     *
     * Endpoint: POST /v1/forced-alignment (multipart: file=audio, text=transcript).
     * Returns per-character and per-word timestamps plus a loss score. Used to
     * rescue timing for eleven_v3 dialog audio (whose own timestamps are broken).
     *
     * @param apiKey ElevenLabs API key
     * @param audio Audio bytes (WAV; same audio you got back from generateDialog)
     * @param contentType MIME type for the audio part (e.g. "audio/wav")
     * @param transcript Tag-stripped transcript that matches the spoken audio
     * @param parentSpan Optional observability span
     */
    Result<ForcedAlignmentResult> forcedAlignment(const std::string &apiKey, const std::vector<uint8_t> &audio,
                                                  const std::string &contentType, const std::string &transcript,
                                                  std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Strip ElevenLabs-style inline audio tags from text, collapse whitespace, trim.
     *
     * Mirrors the dialog prototype's strip_tags: removes "[...]" tags and runs
     * of whitespace become single spaces. The returned string is what gets sent
     * to forced-alignment as the spoken transcript.
     */
    static std::string stripTags(const std::string &text);
};

} // namespace creatures::voice
