#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "StreamingTTSClient.h"
#include "TextToViseme.h"
#include "model/Creature.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/**
 * StreamingAdHocSession
 *
 * Manages a single streaming ad-hoc speech session where the agent sends
 * text sentences incrementally. The session:
 *
 * 1. On start(): opens ElevenLabs WebSocket, sends BOS
 * 2. On addText(): sends each sentence as a text chunk to ElevenLabs
 * 3. On finish(): sends EOS, waits for all audio, builds animation, plays it
 *
 * Audio and alignment data accumulate in the background as ElevenLabs
 * responds to each text chunk. The receiver thread runs continuously
 * from start() until finish() completes.
 */
class StreamingAdHocSession {
  public:
    StreamingAdHocSession(const std::string &sessionId, const std::string &creatureId,
                           bool resumePlaylist, std::shared_ptr<RequestSpan> parentSpan);

    ~StreamingAdHocSession();

    // Non-copyable
    StreamingAdHocSession(const StreamingAdHocSession &) = delete;
    StreamingAdHocSession &operator=(const StreamingAdHocSession &) = delete;

    /**
     * Start the session: look up creature, open ElevenLabs WebSocket, send BOS.
     */
    Result<void> start();

    /**
     * Add a text chunk (sentence) to the stream.
     * Sends it to ElevenLabs as a text frame with try_trigger_generation=true.
     */
    Result<void> addText(const std::string &text);

    /**
     * Finish the session: send EOS, wait for all audio, build animation,
     * trigger playback.
     *
     * Returns the animation ID on success.
     */
    Result<std::string> finish();

    [[nodiscard]] const std::string &getSessionId() const { return sessionId_; }
    [[nodiscard]] int getChunksReceived() const { return chunksReceived_; }

  private:
    std::string sessionId_;
    std::string creatureId_;
    bool resumePlaylist_;
    std::shared_ptr<OperationSpan> span_;

    // Creature data (populated during start())
    Creature creature_;
    nlohmann::json creatureJson_;
    uint16_t audioChannel_ = 1;
    std::string voiceId_;
    std::string modelId_;
    float stability_ = 0.5f;
    float similarityBoost_ = 0.75f;

    // TTS client (lives for duration of session)
    std::unique_ptr<StreamingTTSClient> ttsClient_;

    // Accumulated text for transcript
    std::string fullText_;
    int chunksReceived_ = 0;
};

/**
 * Global registry of active streaming sessions.
 * Thread-safe via mutex.
 */
class StreamingAdHocSessionManager {
  public:
    static StreamingAdHocSessionManager &instance();

    std::shared_ptr<StreamingAdHocSession> createSession(const std::string &creatureId,
                                                          bool resumePlaylist,
                                                          std::shared_ptr<RequestSpan> parentSpan);

    std::shared_ptr<StreamingAdHocSession> getSession(const std::string &sessionId);

    void removeSession(const std::string &sessionId);

  private:
    StreamingAdHocSessionManager() = default;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamingAdHocSession>> sessions_;
};

} // namespace creatures::voice
