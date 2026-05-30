#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TextToViseme.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/**
 * Result from a streaming TTS session.
 *
 * Contains the accumulated audio data and lip sync alignment cues
 * from an ElevenLabs WebSocket streaming session.
 */
struct StreamingTTSResult {
    /// Raw audio data (PCM or MP3 depending on output format)
    std::vector<uint8_t> audioData;

    /// Audio format ("pcm_48000" or "mp3_44100_192")
    std::string audioFormat;

    /// Character-level alignment data from ElevenLabs
    std::vector<TextToViseme::CharTiming> charTimings;

    /// Normalized alignment characters (for debugging)
    std::string alignmentText;

    /// Total audio duration in seconds (estimated from audio data size)
    double audioDurationSeconds = 0.0;

    /// Request ID from ElevenLabs response header (for previous_request_ids chaining)
    std::string requestId;
};

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
 * StreamingTTSClient
 *
 * Connects to the ElevenLabs WebSocket streaming TTS API to generate speech
 * with character-level alignment data in a single streaming call.
 *
 * The WebSocket protocol:
 * 1. Client connects to wss://api.elevenlabs.io/v1/text-to-speech/{voice_id}/stream-input
 * 2. Client sends BOS (begin of stream) message with generation config
 * 3. Client sends text chunks
 * 4. Client sends EOS (end of stream) empty text
 * 5. Server responds with interleaved binary audio chunks and JSON alignment data
 * 6. Connection closes when stream is complete
 *
 * This implementation uses OpenSSL for TLS and manual WebSocket framing
 * since oatpp-openssl is not available in the build.
 */
class StreamingTTSClient {
  public:
    using ProgressCallback = std::function<void(float progress)>;

    StreamingTTSClient();
    ~StreamingTTSClient();

    // Non-copyable
    StreamingTTSClient(const StreamingTTSClient &) = delete;
    StreamingTTSClient &operator=(const StreamingTTSClient &) = delete;

    /**
     * Generate speech via ElevenLabs WebSocket streaming API.
     *
     * Connects, sends text, accumulates audio + alignment data, returns result.
     * This is a blocking call that completes when the stream finishes.
     *
     * @param apiKey ElevenLabs API key
     * @param voiceId Voice ID to use
     * @param modelId Model ID (e.g., "eleven_turbo_v2")
     * @param text Full text to synthesize
     * @param outputFormat Audio output format ("pcm_48000" or "mp3_44100_192")
     * @param stability Voice stability parameter (0.0-1.0)
     * @param similarityBoost Voice similarity boost parameter (0.0-1.0)
     * @param progressCallback Optional progress callback
     * @param parentSpan Optional observability span
     * @return StreamingTTSResult with audio data and alignment cues
     */
    Result<StreamingTTSResult> generateSpeech(const std::string &apiKey, const std::string &voiceId,
                                              const std::string &modelId, const std::string &text,
                                              const std::string &outputFormat, float stability, float similarityBoost,
                                              ProgressCallback progressCallback = nullptr,
                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Generate speech via ElevenLabs REST streaming API with timestamps.
     *
     * Uses POST /v1/text-to-speech/{voice_id}/stream/with-timestamps which
     * supports previous_request_ids for prosody continuity between sentences.
     *
     * @param previousRequestIds Request IDs from prior TTS calls for prosody continuity (max 3)
     */
    Result<StreamingTTSResult> generateSpeechREST(const std::string &apiKey, const std::string &voiceId,
                                                  const std::string &modelId, const std::string &text,
                                                  const std::string &outputFormat, float stability,
                                                  float similarityBoost,
                                                  const std::vector<std::string> &previousRequestIds = {},
                                                  ProgressCallback progressCallback = nullptr,
                                                  std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Generate multi-character dialog via the ElevenLabs Text-to-Dialogue API.
     *
     * Endpoint: POST /v1/text-to-dialogue/with-timestamps?output_format={format}
     * Model is fixed to eleven_v3 — turbo/multilingual are server-rejected for this
     * endpoint with HTTP 400 "does not support dialogue". This deliberately bypasses
     * the eleven_v3 blocklist applied to the ad-hoc single-character path.
     *
     * Returns the single mixed-down audio and voice_segments. Per-creature isolation
     * is the caller's job (slice the mixdown by turn). The voice_segments' character
     * index ranges are reliable on v3; the times are NOT — call forcedAlignment()
     * with the tag-stripped transcript to get real timing.
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
     * of whitespace become single spaces. The returned string is what gets sent to
     * forced-alignment as the spoken transcript.
     */
    static std::string stripTags(const std::string &text);

  private:
    struct SSLConnection;
    std::unique_ptr<SSLConnection> conn_;

    /**
     * Establish TLS connection and perform WebSocket handshake.
     */
    Result<void> connectWebSocket(const std::string &host, uint16_t port, const std::string &path,
                                  const std::string &apiKey, std::shared_ptr<OperationSpan> parentSpan);

    /**
     * Send a WebSocket text frame.
     */
    Result<void> sendTextFrame(const std::string &text);

    /**
     * Send a WebSocket close frame.
     */
    void sendCloseFrame();

    /**
     * Receive and process all WebSocket frames until the connection closes.
     * Accumulates audio data and alignment information.
     */
    Result<StreamingTTSResult> receiveAllFrames(const std::string &outputFormat, ProgressCallback progressCallback,
                                                std::shared_ptr<OperationSpan> parentSpan);

    /**
     * Close the connection and free resources.
     */
    void disconnect();
};

} // namespace creatures::voice
