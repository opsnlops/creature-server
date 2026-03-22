#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "StreamingTTSClient.h"
#include "TextToViseme.h"
#include "model/Animation.h"
#include "model/Creature.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

/**
 * StreamingAdHocSession
 *
 * Manages a pipelined streaming ad-hoc speech session. Each sentence from
 * the agent kicks off a parallel ElevenLabs TTS call immediately. When
 * finish() is called, all TTS results are collected, combined into a single
 * animation with seamless body motion, and played as one uninterrupted piece.
 *
 * The latency win: TTS calls run in parallel while the LLM is still generating.
 * By the time the last sentence arrives, earlier sentences are already TTS'd.
 *
 * 1. start(): looks up creature, loads base animation, prepares for sentences
 * 2. addText(): kicks off ElevenLabs TTS immediately in a background thread
 * 3. finish(): collects all TTS results (most already done), combines audio +
 *    alignment into one animation, triggers single uninterrupted playback
 */
class StreamingAdHocSession {
  public:
    StreamingAdHocSession(const std::string &sessionId, const std::string &creatureId,
                           bool resumePlaylist, std::shared_ptr<RequestSpan> parentSpan);

    ~StreamingAdHocSession();

    StreamingAdHocSession(const StreamingAdHocSession &) = delete;
    StreamingAdHocSession &operator=(const StreamingAdHocSession &) = delete;

    /**
     * Start the session: look up creature, load base animation, validate config.
     */
    Result<void> start();

    /**
     * Add a sentence. Immediately kicks off TTS in a background thread.
     */
    Result<void> addText(const std::string &text);

    /**
     * Collect all TTS results, combine into one animation, trigger playback.
     * Returns the animation ID.
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

    // Base animation data (loaded once in start(), reused for all sentences)
    Animation baseAnimation_;
    std::vector<std::vector<uint8_t>> decodedBaseFrames_;
    uint32_t msPerFrame_ = 1;

    // Universe for playback
    universe_t universe_ = 0;

    // Accumulated text for transcript
    std::string fullText_;
    int chunksReceived_ = 0;

    // TextToViseme (loaded once in start())
    TextToViseme textToViseme_;

    // Futures for in-flight TTS calls (one per sentence)
    std::mutex futuresMutex_;
    std::vector<std::future<Result<StreamingTTSResult>>> sentenceFutures_;
};

/**
 * Global registry of active streaming sessions.
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
