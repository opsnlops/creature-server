#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
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

inline constexpr std::size_t MAX_STREAMING_AD_HOC_CHUNK_TEXT_BYTES = 32 * 1024;
inline constexpr std::size_t MAX_STREAMING_AD_HOC_CHUNKS = 100;
inline constexpr std::size_t MAX_STREAMING_AD_HOC_TOTAL_TEXT_BYTES = 256 * 1024;
inline constexpr std::size_t MAX_ACTIVE_STREAMING_AD_HOC_SESSIONS = 32;
inline constexpr auto STREAMING_AD_HOC_IDLE_TIMEOUT = std::chrono::minutes(10);

/// What finish() learned about the session, for an honest /finish response and
/// the exchange record (issue #150).
struct StreamingFinishResult {
    std::string lastAnimationId; // empty when no sentence rendered
    std::string exchangeStatus;  // ready | partial | failed
    int partsRendered{0};
    int partsTotal{0};
};

/**
 * StreamingAdHocSession
 *
 * Manages a pipelined streaming ad-hoc speech session. Each sentence from
 * the agent kicks off a parallel ElevenLabs TTS call immediately. A background
 * playback thread monitors the futures and triggers playback as soon as each
 * sentence is ready — Beaky starts talking while the LLM is still generating.
 *
 * 1. start(): looks up creature, loads base animation, prepares for sentences
 * 2. addText(): kicks off ElevenLabs TTS immediately in a background thread;
 *    on the first call, also spawns the playback thread
 * 3. finish(): signals no more sentences, waits for playback thread to complete,
 *    then cleans up
 */
class StreamingAdHocSession {
  public:
    StreamingAdHocSession(const std::string &sessionId, const std::string &creatureId, bool resumePlaylist,
                          std::shared_ptr<RequestSpan> parentSpan);

    ~StreamingAdHocSession();

    StreamingAdHocSession(const StreamingAdHocSession &) = delete;
    StreamingAdHocSession &operator=(const StreamingAdHocSession &) = delete;

    /**
     * Start the session: look up creature, load base animation, validate config.
     */
    Result<void> start();

    /**
     * Add a sentence. Immediately kicks off TTS in a background thread.
     * On the first call, also spawns the playback thread that will trigger
     * interrupt() as soon as sentence 1's TTS completes.
     */
    Result<void> addText(const std::string &text, std::shared_ptr<RequestSpan> triggerSpan = nullptr);

    /**
     * Signal that no more sentences are coming. Waits for the playback thread
     * to finish processing all queued animations (every sentence WAV exists on
     * disk once that join returns), then stitches the parts into one exchange
     * WAV with iXML provenance and finalizes the exchange record (issue #150).
     */
    Result<StreamingFinishResult> finish(std::shared_ptr<RequestSpan> triggerSpan = nullptr);

    [[nodiscard]] const std::string &getSessionId() const { return sessionId_; }
    [[nodiscard]] int getChunksReceived() const { return chunksReceived_.load(); }
    [[nodiscard]] bool isIdleExpired(std::chrono::steady_clock::time_point now) const;

  private:
    void touch();
    void resolveFailedSentence(int sentenceIndex) noexcept;
    void renderThreadFunc();
    /// Background thread that monitors futures and triggers playback in order.
    void playbackThreadFunc();

    std::string sessionId_;
    std::string creatureId_;
    bool resumePlaylist_;
    std::shared_ptr<OperationSpan> span_;
    std::atomic<bool> lifecycleCompleted_{false};
    std::atomic<bool> lifecycleFailed_{false};
    std::atomic<int64_t> lastActivityNs_{0};
    std::atomic<std::size_t> outstandingSentenceWork_{0};

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
    std::atomic<int> chunksReceived_{0};
    // Serializes concurrent /text and /finish requests so sentence indexes,
    // transcript order, and the finished transition remain coherent.
    std::mutex stateMutex_;

    // Per-sentence text, in order — becomes the exchange's script provenance.
    std::vector<std::string> sentenceTexts_;

    // What happened to each sentence, recorded by the playback thread (under
    // futuresMutex_) as it consumes the futures in order. Read by finish()
    // after the playback thread joins.
    struct SentenceOutcome {
        bool success{false};
        std::string animationId;
    };
    std::vector<SentenceOutcome> sentenceOutcomes_;

    // TextToViseme (loaded once in start())
    TextToViseme textToViseme_;

    // Futures for in-flight sentence processing (TTS + build, one per sentence)
    // Each future produces a ready-to-play Animation
    std::mutex futuresMutex_;
    std::vector<std::future<Result<Animation>>> sentenceFutures_;
    std::vector<std::shared_ptr<OperationSpan>> sentenceSpans_;

    // One render worker per active session. ElevenLabs prosody continuity
    // already makes sentence pipelines sequential within a session, so a
    // bounded queue preserves useful cross-session parallelism without
    // creating one blocked native thread for every pending sentence.
    std::mutex renderMutex_;
    std::condition_variable renderCv_;
    std::deque<std::packaged_task<Result<Animation>()>> renderTasks_;
    std::thread renderThread_;

    // Condition variable to wake the playback thread when new futures are added
    // or when finish() signals no more sentences.
    std::condition_variable playbackCv_;

    // Playback thread — spawned on first addText(), joins in finish()
    std::thread playbackThread_;
    std::atomic<bool> finished_{false}; // Signals: no more sentences coming
    std::atomic<bool> cancelled_{false};

    // Frame offset synchronization: each sentence waits for the previous one's
    // offset before building, so body motion is seamless. Uses promise/future pairs.
    std::mutex offsetMutex_;
    std::vector<std::promise<size_t>> offsetPromises_;
    std::vector<std::shared_future<size_t>> offsetFutures_;

    // Request ID chaining for ElevenLabs prosody continuity.
    // Each sentence passes its request ID to the next via promise/future.
    std::vector<std::promise<std::string>> requestIdPromises_;
    std::vector<std::shared_future<std::string>> requestIdFutures_;
};

/**
 * Global registry of active streaming sessions.
 */
class StreamingAdHocSessionManager {
  public:
    static StreamingAdHocSessionManager &instance();

    Result<std::shared_ptr<StreamingAdHocSession>> createSession(const std::string &creatureId, bool resumePlaylist,
                                                                 std::shared_ptr<RequestSpan> parentSpan);

    std::shared_ptr<StreamingAdHocSession> getSession(const std::string &sessionId);

    void removeSession(const std::string &sessionId);

  private:
    StreamingAdHocSessionManager() = default;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamingAdHocSession>> sessions_;
};

} // namespace creatures::voice
