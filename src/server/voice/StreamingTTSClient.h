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
                                               const std::string &outputFormat, float stability,
                                               float similarityBoost,
                                               ProgressCallback progressCallback = nullptr,
                                               std::shared_ptr<OperationSpan> parentSpan = nullptr);

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
    Result<StreamingTTSResult> receiveAllFrames(const std::string &outputFormat,
                                                 ProgressCallback progressCallback,
                                                 std::shared_ptr<OperationSpan> parentSpan);

    /**
     * Close the connection and free resources.
     */
    void disconnect();
};

} // namespace creatures::voice
