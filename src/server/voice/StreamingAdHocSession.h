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

#include "GazeTrack.h"
#include "StreamingTTSClient.h"
#include "TextToViseme.h"
#include "model/Animation.h"
#include "model/Creature.h"
#include "model/Stage.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

inline constexpr std::size_t MAX_STREAMING_AD_HOC_CHUNK_TEXT_BYTES = 32 * 1024;
inline constexpr std::size_t MAX_STREAMING_AD_HOC_CHUNKS = 100;
inline constexpr std::size_t MAX_STREAMING_AD_HOC_TOTAL_TEXT_BYTES = 256 * 1024;
inline constexpr std::size_t MAX_ACTIVE_STREAMING_AD_HOC_SESSIONS = 32;
inline constexpr std::size_t MAX_PENDING_STREAMING_AD_HOC_SENTENCES = 16;
inline constexpr std::size_t MAX_GLOBAL_STREAMING_AD_HOC_RENDERS = 128;
inline constexpr auto STREAMING_AD_HOC_IDLE_TIMEOUT = std::chrono::minutes(10);
inline constexpr auto STREAMING_AD_HOC_ABSOLUTE_TIMEOUT = std::chrono::minutes(30);
inline constexpr auto STREAMING_AD_HOC_CLAIM_GRACE = std::chrono::seconds(5);
/// Most creatures one streamed dialog can seat (issue #186). Mirrors the
/// dialog job's per-scene voice cap.
inline constexpr std::size_t MAX_STREAMING_DIALOG_PARTICIPANTS = 8;

/// What finish() learned about the session, for an honest /finish response and
/// the exchange record (issue #150).
struct StreamingFinishResult {
    std::string lastAnimationId;     // empty when no sentence rendered
    std::string exchangeAnimationId; // the whole exchange as one N-track animation; dialog sessions only (#186)
    std::string exchangeStatus;      // ready | partial | failed
    int partsRendered{0};
    int partsTotal{0};
};

/// What /start asked for. One creature and no stage is the classic ad-hoc
/// stream; several creatures on a stage is a streamed dialog (issue #186).
struct StreamingSessionConfig {
    std::vector<std::string> creatureIds; // participants, in the order they were given
    std::string stageId;                  // required for dialog sessions; empty = no head aiming
    bool resumePlaylist{true};
    // A dialog session records every participant on the exchange, names the
    // speaker of each part, and stitches the whole exchange into one N-track
    // ad-hoc animation at /finish. Off for /ad-hoc-stream, whose records and
    // responses must keep their pre-#186 shape.
    bool dialog{false};
};

/**
 * StreamingAdHocSession
 *
 * Manages a pipelined streaming speech session for one or more creatures.
 * Each turn from the agent kicks off an ElevenLabs TTS call immediately. A
 * background playback thread monitors the futures and triggers playback as
 * soon as each turn is ready — Beaky starts talking while the LLM is still
 * generating.
 *
 * With several participants (issue #186) every turn is rendered as one
 * animation carrying a track per participant: the speaker's speech loop with
 * its mouth driven, and each listener cycling its idle loop with its beak
 * shut. Per-creature continuity (body-loop phase, idle phase, ElevenLabs
 * prosody chaining, head aiming) is carried from turn to turn so the sequence
 * of turns plays like one scene. A single participant is exactly the classic
 * ad-hoc stream.
 *
 * 1. start(): looks up creatures, loads base animations, prepares for turns
 * 2. addText()/addTurn(): kicks off ElevenLabs TTS immediately in a background
 *    thread; on the first call, also spawns the playback thread
 * 3. finish(): signals no more turns, waits for playback thread to complete,
 *    then stitches the exchange and cleans up
 */
class StreamingAdHocSession {
  public:
    StreamingAdHocSession(const std::string &sessionId, StreamingSessionConfig config,
                          std::shared_ptr<RequestSpan> parentSpan);

    ~StreamingAdHocSession();

    StreamingAdHocSession(const StreamingAdHocSession &) = delete;
    StreamingAdHocSession &operator=(const StreamingAdHocSession &) = delete;

    /**
     * Start the session: look up every participant, load base animations,
     * validate config, bind the stage.
     */
    Result<void> start();

    /**
     * Add a sentence spoken by the session's only participant — the classic
     * ad-hoc stream. Refused (Conflict) on a session with several
     * participants: those must say who is speaking via addTurn().
     */
    Result<void> addText(const std::string &text, std::shared_ptr<RequestSpan> triggerSpan = nullptr);

    /**
     * Add one turn: a sentence spoken by `creatureId`, which must be a
     * participant (InvalidData otherwise). Immediately kicks off TTS in a
     * background thread. On the first call, also spawns the playback thread
     * that will trigger interrupt() as soon as turn 1's TTS completes.
     */
    Result<void> addTurn(const std::string &creatureId, const std::string &text,
                         std::shared_ptr<RequestSpan> triggerSpan = nullptr);

    /**
     * Signal that no more sentences are coming. Waits for the playback thread
     * to finish processing all queued animations (every sentence WAV exists on
     * disk once that join returns), then stitches the parts into one exchange
     * WAV with iXML provenance and finalizes the exchange record (issue #150).
     */
    Result<StreamingFinishResult> finish(std::shared_ptr<RequestSpan> triggerSpan = nullptr);

    [[nodiscard]] const std::string &getSessionId() const { return sessionId_; }
    [[nodiscard]] int getChunksReceived() const { return chunksReceived_.load(); }
    [[nodiscard]] std::size_t getParticipantCount() const { return config_.creatureIds.size(); }
    [[nodiscard]] bool tryRenewClientLease(std::chrono::steady_clock::time_point now);
    [[nodiscard]] bool tryExpire(std::chrono::steady_clock::time_point now);

  private:
    /// Everything start() learns about one creature in the session.
    struct Participant {
        std::string creatureId;
        Creature creature;
        nlohmann::json creatureJson;
        std::string name;
        uint16_t audioChannel{1};
        std::string voiceId;
        std::string modelId;
        float stability{0.5f};
        float similarityBoost{0.75f};
        std::size_t mouthSlot{0};

        // Speech loop: cycled while this creature speaks.
        std::vector<std::vector<uint8_t>> speechFrames;
        Animation baseAnimation;
        std::string speechAnimationId;

        // Idle loop: cycled while this creature listens (#119). Empty when
        // the creature has none usable → frozen on speechFrames[0].
        std::vector<std::vector<uint8_t>> idleFrames;
        std::string idleAnimationId;
        std::size_t idleStartOffset{0};

        // Head aiming (#119), only when a stage is bound.
        GazeGeometry gaze;
    };

    /// Where one creature's body, voice and head were left by the previous
    /// turn — what the next turn needs to carry on seamlessly.
    struct CreatureContinuity {
        bool primed{false};
        bool wasSpeaking{false};
        std::size_t speechOffset{0};
        std::size_t idleOffset{0};
        std::vector<uint8_t> lastBodyFrame;
        std::string lastRequestId; // ElevenLabs prosody chaining, per voice
        GazeContinuity gaze;
    };
    struct TurnContinuity {
        std::vector<CreatureContinuity> creatures; // parallel to participants_
    };

    /// What the render pipeline hands the playback thread for one turn.
    struct RenderedTurn {
        Animation animation;
        uint64_t audioSamples{0}; // mono samples at 48 kHz, for the stitched track sizing
        std::string requestId;    // ElevenLabs request id, for provenance
    };

    void touchClientActivity();
    [[nodiscard]] TurnContinuity initialContinuity() const;
    [[nodiscard]] TurnContinuity continuityBefore(int sentenceIndex);
    void resolveFailedSentence(int sentenceIndex, const TurnContinuity &carry) noexcept;
    void renderThreadFunc();
    /// Background thread that monitors futures and triggers playback in order.
    void playbackThreadFunc();

    std::string sessionId_;
    StreamingSessionConfig config_;
    std::shared_ptr<OperationSpan> span_;
    std::atomic<bool> lifecycleCompleted_{false};
    std::atomic<bool> lifecycleFailed_{false};
    int64_t createdAtNs_{0};
    int64_t lastClientActivityNs_{0};
    int64_t clientClaimUntilNs_{0};
    std::atomic<std::size_t> outstandingSentenceWork_{0};

    // Participants (populated during start()), in /start order.
    std::vector<Participant> participants_;
    // Every participant's speech loop runs at this rate; start() refuses a
    // mixed-rate cast, as the dialog job does.
    uint32_t msPerFrame_ = 1;

    // Stage binding for head aiming (#119, required for dialogs by #186).
    bool haveStage_{false};
    Stage stage_;
    std::vector<GazeGeometry> gazeGeometries_; // parallel to participants_
    // Per-creature reaction timing. Only the render thread draws from these,
    // and it renders turns one at a time, so no lock is needed.
    std::vector<std::mt19937> gazeRngs_;
    // Seeds every random choice the session makes (loop picks, idle phases,
    // gaze timing); stamped on the stitched animation like the dialog job.
    uint64_t renderSeed_{0};

    // Universe for playback
    universe_t universe_ = 0;

    // Accumulated text for transcript
    std::string fullText_;
    std::atomic<int> chunksReceived_{0};
    // Serializes concurrent /text and /finish requests so sentence indexes,
    // transcript order, and the finished transition remain coherent.
    std::mutex stateMutex_;

    // Per-turn record, in order — becomes the exchange's script provenance.
    struct TurnRecord {
        std::size_t speakerIndex{0}; // into participants_
        std::string text;
    };
    std::vector<TurnRecord> turns_;

    // What happened to each sentence, recorded by the playback thread (under
    // futuresMutex_) as it consumes the futures in order. Read by finish()
    // after the playback thread joins.
    struct SentenceOutcome {
        bool success{false};
        std::string animationId;
        std::string requestId;
        uint64_t audioSamples{0};
        std::vector<Track> tracks; // kept only for dialog sessions, for the stitched animation
    };
    std::vector<SentenceOutcome> sentenceOutcomes_;

    // TextToViseme (loaded once in start())
    TextToViseme textToViseme_;

    // Futures for in-flight sentence processing (TTS + build, one per sentence)
    // Each future produces a ready-to-play turn
    std::mutex futuresMutex_;
    std::vector<std::future<Result<RenderedTurn>>> sentenceFutures_;
    std::vector<std::shared_ptr<OperationSpan>> sentenceSpans_;

    // One render worker per active session. ElevenLabs prosody continuity
    // already makes sentence pipelines sequential within a session, so a
    // bounded queue preserves useful cross-session parallelism without
    // creating one blocked native thread for every pending sentence.
    std::mutex renderMutex_;
    std::condition_variable renderCv_;
    std::deque<std::packaged_task<Result<RenderedTurn>()>> renderTasks_;
    std::thread renderThread_;

    // Condition variable to wake the playback thread when new futures are added
    // or when finish() signals no more sentences.
    std::condition_variable playbackCv_;

    // Playback thread — spawned on first addText(), joins in finish()
    std::thread playbackThread_;
    std::atomic<bool> finished_{false}; // Signals: no more sentences coming
    std::atomic<bool> cancelled_{false};

    // Continuity chain: each turn waits for the previous turn's snapshot
    // before building, so body motion, idle phase, prosody and head aiming
    // are seamless across turns. Uses promise/future pairs. A failed turn
    // forwards the snapshot it received, so the chain never stalls.
    std::mutex offsetMutex_;
    std::vector<std::promise<TurnContinuity>> continuityPromises_;
    std::vector<std::shared_future<TurnContinuity>> continuityFutures_;
};

/**
 * Global registry of active streaming sessions.
 */
class StreamingAdHocSessionManager {
  public:
    static StreamingAdHocSessionManager &instance();

    /// The classic single-creature ad-hoc stream.
    Result<std::shared_ptr<StreamingAdHocSession>> createSession(const std::string &creatureId, bool resumePlaylist,
                                                                 std::shared_ptr<RequestSpan> parentSpan);

    /// Any session shape, including a streamed dialog (issue #186).
    Result<std::shared_ptr<StreamingAdHocSession>> createSession(StreamingSessionConfig config,
                                                                 std::shared_ptr<RequestSpan> parentSpan);

    std::shared_ptr<StreamingAdHocSession> getSession(const std::string &sessionId);

    void removeSession(const std::string &sessionId);

  private:
    StreamingAdHocSessionManager() = default;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<StreamingAdHocSession>> sessions_;
};

} // namespace creatures::voice
