#pragma once

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
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
 * the agent is processed independently through the full TTS→animation pipeline,
 * and animations are chained back-to-back for seamless playback.
 *
 * 1. start(): looks up creature, loads base animation, prepares for sentences
 * 2. addText(): kicks off TTS+animation pipeline for this sentence immediately
 *    in a background thread — first sentence triggers interrupt(), subsequent
 *    sentences queue for chained playback
 * 3. finish(): waits for all in-flight sentences to complete, returns final animation ID
 *
 * The base animation frame offset is tracked across sentences so body motion
 * continues seamlessly — only the mouth data changes between sentence animations.
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
     * Add a sentence. Immediately kicks off TTS+animation in a background thread.
     * First sentence triggers playback; subsequent sentences chain seamlessly.
     */
    Result<void> addText(const std::string &text);

    /**
     * Wait for all in-flight sentences to finish processing and playing.
     * Returns the last animation ID.
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

    // Track base animation frame offset for seamless body motion
    std::atomic<size_t> baseFrameOffset_{0};

    // Track whether this is the first sentence (triggers interrupt vs chain)
    std::atomic<bool> firstSentence_{true};

    // Accumulated text for transcript
    std::string fullText_;
    int chunksReceived_ = 0;
    std::string lastAnimationId_;

    // TextToViseme (loaded once in start())
    TextToViseme textToViseme_;

    // Futures for in-flight sentence processing
    std::mutex futuresMutex_;
    std::vector<std::future<Result<std::string>>> sentenceFutures_;

    // Pending animations queue for chained playback
    std::mutex pendingMutex_;
    std::queue<Animation> pendingAnimations_;
    std::condition_variable pendingCv_;

    /**
     * Process a single sentence through the full pipeline:
     * TTS → MP3→WAV → lip sync → Opus → animation build → playback
     */
    Result<std::string> processSentence(const std::string &text, int sentenceIndex);

    /**
     * Build an animation from TTS result, using the shared base animation
     * with the current frame offset for seamless body motion.
     */
    Result<Animation> buildSentenceAnimation(const StreamingTTSResult &ttsData,
                                              const std::string &text, int sentenceIndex,
                                              std::shared_ptr<OperationSpan> parentSpan);

    /**
     * Schedule the next pending animation for playback.
     * Called from the previous animation's onFinish callback.
     */
    void playNextPending();
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
