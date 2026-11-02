#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TextToViseme.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/**
 * Which ElevenLabs socket a payload came from.
 *
 * The two sockets carry the same information under different spellings and
 * nothing in the payload says which one you are on, so the caller has to say.
 * Text-to-speech ends a stream with `isFinal` and spells alignment fields in
 * camelCase; text-to-dialogue ends each turn with `is_final_audio_for_turn`
 * and spells them in snake_case (issue #248).
 */
enum class WireDialect {
    TextToSpeech,
    Dialogue,
};

/// How one socket reports alignment: the field names, and whether the times in
/// each chunk are absolute or start again from zero.
struct WireKeys {
    const char *final;       ///< boolean field marking the end of a stream or turn
    const char *starts;      ///< alignment array of character start times, ms
    const char *durations;   ///< alignment array of character durations, ms
    bool chunkRelativeTimes; ///< true when each chunk's times restart at zero
};

/// Field names and timing convention for a socket. The dialects differ in
/// spelling AND in something far easier to miss: the text-to-speech socket
/// reports absolute times, so chunks concatenate directly, while the
/// text-to-dialogue socket restarts every chunk at zero. Concatenating the
/// latter crams a whole utterance into its first chunk's worth of
/// milliseconds, which reads as a bird that barely opens its beak (issue #252).
constexpr WireKeys wireKeys(const WireDialect dialect) {
    if (dialect == WireDialect::Dialogue) {
        return WireKeys{"is_final_audio_for_turn", "char_start_times_ms", "char_durations_ms", true};
    }
    return WireKeys{"isFinal", "charStartTimesMs", "charDurationsMs", false};
}

/**
 * Result from a streaming TTS session.
 *
 * Contains the accumulated audio data and lip sync alignment cues
 * from an ElevenLabs WebSocket streaming session.
 */
/// Seconds of audio in `bytes` of the requested ElevenLabs output format. PCM
/// is 16-bit mono at the rate named in the format ("pcm_48000"); a fixed
/// 44.1 kHz divisor here made every pcm_48000 sentence ~8.8% too long, which
/// held the body moving in silence and delayed the next turn (issue #189).
/// MP3 keeps the rough 24 kB/s estimate: nothing in the streaming path renders
/// frames from it. Unknown formats estimate 0.
inline double estimateAudioSeconds(const std::string &outputFormat, std::size_t bytes) {
    if (outputFormat.rfind("pcm_", 0) == 0) {
        unsigned long sampleRate = 0;
        try {
            sampleRate = std::stoul(outputFormat.substr(4));
        } catch (const std::exception &) {
            sampleRate = 0;
        }
        if (sampleRate == 0) {
            return 0.0;
        }
        return static_cast<double>(bytes) / (static_cast<double>(sampleRate) * 2.0);
    }
    if (outputFormat.find("mp3") != std::string::npos) {
        return static_cast<double>(bytes) / 24000.0;
    }
    return 0.0;
}

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
 * Uses IXWebSocket, as the dialogue client and creature-controller do. It
 * previously hand-rolled TLS and framing over OpenSSL because the transport of
 * the day offered no reusable TLS client; that is no longer true, and roughly
 * 450 lines of handshake, masking, fragment reassembly and ping/pong went away
 * with it (issue #251).
 */
/**
 * Pull one alignment object apart into character timings.
 *
 * Free and inline so both sockets share one parser and both dialects can be
 * tested without a socket — the same reason estimateAudioSeconds lives here.
 * Appends to `charTimings` and `alignmentText`, and returns how many characters
 * it took.
 *
 * `runningOffsetMs` carries the position of this chunk within the utterance.
 * On a socket whose chunks restart at zero it is added to every start time and
 * then advanced past this chunk; on one reporting absolute times it is left
 * alone. Callers keep one per utterance and reset it between them — getting
 * this wrong does not fail, it just stacks the whole utterance on top of
 * itself (issue #252).
 *
 * Returns 0 when the object lacks the fields that dialect expects, which is
 * exactly what a dialect mix-up looks like: a perfectly well-formed stream
 * with no lip sync at all (issue #248).
 */
inline std::size_t appendAlignmentChunk(const nlohmann::json &alignment, const WireDialect dialect,
                                        std::vector<TextToViseme::CharTiming> &charTimings, std::string &alignmentText,
                                        double &runningOffsetMs) {
    if (!alignment.is_object()) {
        return 0;
    }
    const auto keys = wireKeys(dialect);
    if (!alignment.contains("chars") || !alignment.contains(keys.starts) || !alignment.contains(keys.durations)) {
        return 0;
    }
    const auto chars = alignment["chars"].get<std::vector<std::string>>();
    const auto starts = alignment[keys.starts].get<std::vector<double>>();
    const auto durs = alignment[keys.durations].get<std::vector<double>>();
    const std::size_t count = std::min({chars.size(), starts.size(), durs.size()});
    const double offset = keys.chunkRelativeTimes ? runningOffsetMs : 0.0;
    double extent = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        TextToViseme::CharTiming timing;
        timing.character = chars[i].empty() ? ' ' : chars[i][0];
        timing.startTimeMs = starts[i] + offset;
        timing.durationMs = durs[i];
        charTimings.push_back(timing);
        alignmentText.push_back(timing.character);
        extent = std::max(extent, starts[i] + durs[i]);
    }
    if (keys.chunkRelativeTimes) {
        // The next chunk begins where this one ended.
        runningOffsetMs += extent;
    }
    return count;
}

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

  private:
    /// Called on IXWebSocket's thread for each frame of a stream.
    void onFrame(const std::string &payload, bool binary);

    // One sentence per connection, so this state belongs to the single
    // generateSpeech() call in flight. IXWebSocket delivers frames on its own
    // thread; the caller blocks on cv_ until that thread says the stream
    // finished.
    std::mutex mutex_;
    std::condition_variable cv_;
    StreamingTTSResult pending_;
    bool open_{false};
    bool complete_{false};
    std::string failure_;
    /// Unused by this socket's absolute times, but the parser takes one.
    double alignmentOffsetMs_{0.0};
};

} // namespace creatures::voice
