#include "StreamingAdHocSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <unordered_map>

#include <base64.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "PcmWavWriter.h"
#include "RhubarbData.h"
#include "SoundDataProcessor.h"
#include "TurnConcat.h"
#include "model/AdHocExchange.h"
#include "model/Animation.h"
#include "server/animation/CooperativeAnimationScheduler.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/creature/UniverseResolver.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/storage/Storage.h"
#include "server/voice/SpeechTrackBuilder.h"
#include "util/Slugify.h"
#include "util/ThreadPriority.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/uuidUtils.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<util::AudioCache> audioCache;
} // namespace creatures

namespace creatures::voice {

namespace {

std::atomic<std::size_t> globalReservedRenders{0};

class GlobalRenderReservation {
  public:
    ~GlobalRenderReservation() { globalReservedRenders.fetch_sub(1, std::memory_order_release); }
};

std::unique_ptr<GlobalRenderReservation> tryReserveGlobalRender() {
    auto reserved = globalReservedRenders.load(std::memory_order_relaxed);
    while (reserved < MAX_GLOBAL_STREAMING_AD_HOC_RENDERS) {
        if (globalReservedRenders.compare_exchange_weak(reserved, reserved + 1, std::memory_order_acquire,
                                                        std::memory_order_relaxed)) {
            return std::make_unique<GlobalRenderReservation>();
        }
    }
    return nullptr;
}

int64_t monotonicNowNs(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

} // namespace

// --- StreamingAdHocSession ---

StreamingAdHocSession::StreamingAdHocSession(const std::string &sessionId, StreamingSessionConfig config,
                                             std::shared_ptr<RequestSpan> parentSpan)
    : sessionId_(sessionId), config_(std::move(config)) {

    createdAtNs_ = monotonicNowNs();
    lastClientActivityNs_ = createdAtNs_;

    if (parentSpan && creatures::observability) {
        span_ = creatures::observability->createLinkedOperationSpan("StreamingAdHocSession", parentSpan);
        if (span_) {
            span_->setAttribute("session.id", sessionId);
            span_->setAttribute("creature.id", config_.creatureIds.empty() ? "" : config_.creatureIds.front());
            span_->setAttribute("session.participants", static_cast<int64_t>(config_.creatureIds.size()));
            span_->setAttribute("session.dialog", config_.dialog);
            if (!config_.stageId.empty())
                span_->setAttribute("stage.id", config_.stageId);
        }
    }

    std::string creatureList;
    for (const auto &creatureId : config_.creatureIds) {
        if (!creatureList.empty())
            creatureList += ",";
        creatureList += creatureId;
    }
    info("StreamingAdHocSession created: session={}, creatures={}", sessionId, creatureList);
}

StreamingAdHocSession::~StreamingAdHocSession() {
    // Ensure both workers are joined if the manager expires or shuts down a
    // session that the client abandoned.
    cancelled_.store(true);
    finished_.store(true);
    renderCv_.notify_one();
    playbackCv_.notify_one();
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }
    joinCachePublishes();

    debug("StreamingAdHocSession destroyed: session={}", sessionId_);
    if (span_) {
        if (lifecycleCompleted_.load() && lifecycleFailed_.load()) {
            span_->setAttribute("session.outcome", "partial");
            recordSpanError(span_, "Streaming session completed with failed sentences or playback",
                            "DegradedStreamingSession", ServerError::InternalError);
        } else if (lifecycleCompleted_.load()) {
            span_->setAttribute("session.outcome", "completed");
            span_->setSuccess();
        } else if (lifecycleFailed_.load()) {
            span_->setAttribute("session.outcome", "failed");
            recordSpanError(span_, "Streaming session failed before terminal completion", "FailedStreamingSession",
                            ServerError::InternalError);
        } else {
            span_->setAttribute("session.outcome", "abandoned");
            recordSpanError(span_, "Streaming session ended without finish", "AbandonedSession", ServerError::Conflict);
        }
    }
}

void StreamingAdHocSession::touchClientActivity() { lastClientActivityNs_ = monotonicNowNs(); }

void StreamingAdHocSession::publishAudioCacheInBackground(
    std::shared_ptr<creatures::rtp::AudioStreamBuffer> audioBuffer, std::string wavPath,
    std::shared_ptr<OperationSpan> parentSpan) {
    // The turn is scheduled; the cache write can take its time. It keeps a
    // later playback of this sentence's WAV (the exchange list, the console)
    // on the fast path, which is why it is still done at all (issue #197).
    std::lock_guard<std::mutex> lock(cachePublishMutex_);
    cachePublishThreads_.emplace_back(
        [audioBuffer = std::move(audioBuffer), wavPath = std::move(wavPath), parentSpan = std::move(parentSpan)] {
            util::lowerCurrentThreadPriority();
            auto span = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                       "StreamingAdHocSession.publishAudioCache", parentSpan)
                                                 : nullptr;
            auto result = audioBuffer->publishToDiskCache(wavPath, span);
            if (!result.isSuccess()) {
                warn("Deferred audio cache publish for {} failed: {}", wavPath, result.getError()->getMessage());
            } else if (span) {
                span->setSuccess();
            }
        });
}

void StreamingAdHocSession::joinCachePublishes() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(cachePublishMutex_);
        threads.swap(cachePublishThreads_);
    }
    for (auto &thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

bool StreamingAdHocSession::tryRenewClientLease(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (cancelled_.load(std::memory_order_acquire))
        return false;
    if (finished_.load(std::memory_order_acquire))
        return true;

    const auto nowNs = monotonicNowNs(now);
    const auto absoluteDeadlineNs =
        createdAtNs_ + std::chrono::duration_cast<std::chrono::nanoseconds>(STREAMING_AD_HOC_ABSOLUTE_TIMEOUT).count();
    const bool idleExpired =
        nowNs - lastClientActivityNs_ >=
        std::chrono::duration_cast<std::chrono::nanoseconds>(STREAMING_AD_HOC_IDLE_TIMEOUT).count();
    const bool absoluteExpired = nowNs >= absoluteDeadlineNs;
    if (idleExpired || absoluteExpired) {
        if (span_) {
            span_->setAttribute("session.cancellation.reason", idleExpired ? "idle_timeout" : "absolute_timeout");
            span_->setAttribute("session.age.ms", (nowNs - createdAtNs_) / 1'000'000);
        }
        cancelled_.store(true, std::memory_order_release);
        finished_.store(true, std::memory_order_release);
        renderCv_.notify_one();
        playbackCv_.notify_one();
        return false;
    }
    lastClientActivityNs_ = nowNs;
    clientClaimUntilNs_ = std::min(
        absoluteDeadlineNs + std::chrono::duration_cast<std::chrono::nanoseconds>(STREAMING_AD_HOC_CLAIM_GRACE).count(),
        nowNs + std::chrono::duration_cast<std::chrono::nanoseconds>(STREAMING_AD_HOC_CLAIM_GRACE).count());
    return true;
}

bool StreamingAdHocSession::tryExpire(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (cancelled_.load(std::memory_order_acquire))
        return true;
    if (finished_.load(std::memory_order_acquire))
        return false;

    const auto nowNs = monotonicNowNs(now);
    if (nowNs < clientClaimUntilNs_)
        return false;
    const bool idleExpired =
        nowNs - lastClientActivityNs_ >=
        std::chrono::duration_cast<std::chrono::nanoseconds>(STREAMING_AD_HOC_IDLE_TIMEOUT).count();
    const bool absoluteExpired =
        nowNs - createdAtNs_ >=
        std::chrono::duration_cast<std::chrono::nanoseconds>(STREAMING_AD_HOC_ABSOLUTE_TIMEOUT).count();
    if (!idleExpired && !absoluteExpired)
        return false;

    if (span_) {
        span_->setAttribute("session.cancellation.reason", idleExpired ? "idle_timeout" : "absolute_timeout");
        span_->setAttribute("session.age.ms", (nowNs - createdAtNs_) / 1'000'000);
    }
    cancelled_.store(true, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
    renderCv_.notify_one();
    playbackCv_.notify_one();
    return true;
}

StreamingAdHocSession::TurnContinuity StreamingAdHocSession::initialContinuity() const {
    TurnContinuity continuity;
    continuity.creatures.resize(participants_.size());
    for (std::size_t i = 0; i < participants_.size(); ++i) {
        continuity.creatures[i].idleOffset = participants_[i].idleStartOffset;
    }
    return continuity;
}

StreamingAdHocSession::TurnContinuity StreamingAdHocSession::continuityBefore(int sentenceIndex) {
    // The snapshot turn N would have started from: turn N-1's, when it has
    // resolved (the single render thread guarantees it has by the time the
    // playback thread sees N's future), else the session's opening state.
    if (sentenceIndex > 1) {
        std::shared_future<TurnContinuity> previousFuture;
        {
            std::lock_guard<std::mutex> lock(offsetMutex_);
            if (static_cast<std::size_t>(sentenceIndex - 2) < continuityFutures_.size())
                previousFuture = continuityFutures_[sentenceIndex - 2];
        }
        if (previousFuture.valid() && previousFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                return previousFuture.get();
            } catch (const std::exception &) {
            }
        }
    }
    return initialContinuity();
}

void StreamingAdHocSession::resolveFailedSentence(int sentenceIndex, const TurnContinuity &carry) noexcept {
    // A failed turn changes nothing about where anyone's body or head is, so
    // the next turn continues from the snapshot this one received.
    std::lock_guard<std::mutex> lock(offsetMutex_);
    try {
        continuityPromises_.at(static_cast<std::size_t>(sentenceIndex - 1)).set_value(carry);
    } catch (const std::exception &) {
    }
}

Result<void> StreamingAdHocSession::start() {
    auto startSpan = creatures::observability
                         ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.start", span_)
                         : nullptr;
    const auto fail = [&](const ServerError &failure, const char *errorType) -> Result<void> {
        lifecycleFailed_.store(true);
        recordSpanError(startSpan, failure.getMessage(), errorType, failure.getCode());
        recordSpanError(span_, failure.getMessage(), errorType, failure.getCode());
        return Result<void>{failure};
    };

    if (config_.creatureIds.empty()) {
        return fail(ServerError(ServerError::InvalidData, "A streaming session needs at least one creature"),
                    "NoParticipants");
    }
    if (config_.creatureIds.size() > MAX_STREAMING_DIALOG_PARTICIPANTS) {
        return fail(ServerError(ServerError::InvalidData, fmt::format("A streaming dialog seats at most {} creatures",
                                                                      MAX_STREAMING_DIALOG_PARTICIPANTS)),
                    "TooManyParticipants");
    }
    if (config_.dialog && config_.stageId.empty()) {
        return fail(ServerError(ServerError::InvalidData, "A streaming dialog must be bound to a stage"),
                    "MissingStage");
    }

    // One seed for every random choice the session makes (loop picks, idle
    // phases, gaze reaction timing), stamped on the stitched animation so a
    // stage re-render of the exchange reproduces the same body motion (#119).
    renderSeed_ = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::mt19937 rng(static_cast<uint32_t>(renderSeed_));

    // Streaming needs a model that serves audio as it renders.
    static const std::vector<std::string> nonStreamingModels = {"eleven_v3", "eleven_multilingual_v2",
                                                                "eleven_monolingual_v1", "eleven_multilingual_v1"};

    participants_.clear();
    participants_.reserve(config_.creatureIds.size());
    std::unordered_map<uint16_t, std::string> channelOwner;
    for (const auto &creatureId : config_.creatureIds) {
        Participant participant;
        participant.creatureId = creatureId;

        // Look up creature
        auto creatureJsonResult = creatures::db->getCreatureJson(creatureId, startSpan);
        if (!creatureJsonResult.isSuccess()) {
            return fail(creatureJsonResult.getError().value(), "CreatureLookupFailed");
        }
        participant.creatureJson = creatureJsonResult.getValue().value();

        auto creatureResult = creatures::db->getCreature(creatureId, startSpan);
        if (!creatureResult.isSuccess()) {
            return fail(creatureResult.getError().value(), "CreatureLookupFailed");
        }
        participant.creature = creatureResult.getValue().value();
        participant.name = participant.creature.name.empty() ? creatureId : participant.creature.name;

        if (!participant.creatureJson.contains("voice") || participant.creatureJson["voice"].is_null()) {
            return fail(
                ServerError(ServerError::InvalidData, fmt::format("No voice config for creature {}", creatureId)),
                "MissingVoiceConfig");
        }

        // Extract voice config
        try {
            participant.audioChannel = participant.creatureJson.value("audio_channel", static_cast<uint16_t>(1));
            auto voiceConfig = participant.creatureJson["voice"];
            participant.voiceId = voiceConfig["voice_id"].get<std::string>();
            participant.modelId = voiceConfig["model_id"].get<std::string>();
            participant.stability = voiceConfig["stability"].get<float>();
            participant.similarityBoost = voiceConfig["similarity_boost"].get<float>();
        } catch (const std::exception &e) {
            if (startSpan)
                startSpan->recordException(e);
            if (span_)
                span_->recordException(e);
            return fail(ServerError(ServerError::InvalidData,
                                    fmt::format("Bad voice config for creature {}: {}", creatureId, e.what())),
                        "InvalidVoiceConfig");
        }

        // Validate model supports streaming
        for (const auto &blocked : nonStreamingModels) {
            if (participant.modelId == blocked) {
                // Name the creature: in a cast of several this is the whole
                // diagnosis, and the world only sees the message.
                return fail(ServerError(ServerError::InvalidData,
                                        fmt::format("{}'s voice model '{}' does not support streaming; use a "
                                                    "streaming-capable model such as eleven_turbo_v2_5",
                                                    participant.name, participant.modelId)),
                            "UnsupportedVoiceModel");
            }
        }

        // Two creatures on one audio lane would talk over each other in the
        // 17-channel WAV; the dialog job refuses this too.
        if (const auto [owner, inserted] = channelOwner.emplace(participant.audioChannel, participant.name);
            !inserted) {
            return fail(ServerError(ServerError::InvalidData,
                                    fmt::format("audio_channel {} is assigned to both {} and {}",
                                                participant.audioChannel, owner->second, participant.name)),
                        "DuplicateAudioChannel");
        }

        participant.mouthSlot = creatures::resolvedMouthSlot(participant.creature);

        // Resolve speech-loop base frames via the shared helper (issue #15).
        // Returns the decoded body track + the base animation's id + ms-per-frame.
        auto resolveResult = resolveSpeechBaseFrames(participant.creature, *creatures::db, rng, startSpan);
        if (!resolveResult.isSuccess()) {
            return fail(resolveResult.getError().value(), "SpeechBaseResolutionFailed");
        }
        auto resolved = resolveResult.getValue().value();
        participant.speechFrames = std::move(resolved.baseFrames);
        participant.baseAnimation = std::move(resolved.baseAnimation);
        participant.speechAnimationId = resolved.baseAnimationId;
        const uint32_t participantMsPerFrame = resolved.baseMsPerFrame == 0 ? 1u : resolved.baseMsPerFrame;
        if (participants_.empty()) {
            msPerFrame_ = participantMsPerFrame;
        } else if (participantMsPerFrame != msPerFrame_) {
            // Every track of one animation runs at one rate; mixing would play
            // someone's loop at the wrong speed. Same rule as the dialog job.
            return fail(ServerError(ServerError::InvalidData,
                                    fmt::format("{}'s speech loop runs at {} ms/frame but the session is {} ms/frame; "
                                                "mixed-rate dialogs are not supported",
                                                participant.name, participantMsPerFrame, msPerFrame_)),
                        "MixedFrameRate");
        }

        // Idle loop for the listening stretches (#119). Only a cast of more
        // than one ever listens, so a lone creature does no extra DB work.
        // Best-effort: a creature that can't idle freezes on its rest pose.
        if (config_.creatureIds.size() > 1 && !participant.creature.idle_animation_ids.empty()) {
            auto idleResult = resolveIdleBaseFrames(participant.creature, *creatures::db, rng, startSpan);
            if (!idleResult.isSuccess()) {
                warn("{}: idle loop failed to resolve ({}); freezing while listening instead", participant.name,
                     idleResult.getError()->getMessage());
            } else {
                auto idle = idleResult.getValue().value();
                const uint32_t idleMsPerFrame = idle.baseMsPerFrame == 0 ? 1u : idle.baseMsPerFrame;
                if (idleMsPerFrame != msPerFrame_) {
                    warn("{}: idle anim {} is {} ms/frame but the session is {}; freezing while listening instead",
                         participant.name, idle.baseAnimationId, idleMsPerFrame, msPerFrame_);
                } else if (idle.baseFrames.empty() ||
                           idle.baseFrames.front().size() != participant.speechFrames.front().size()) {
                    warn("{}: idle anim {} frames unusable ({} frames, width {} vs speech width {}); freezing while "
                         "listening instead",
                         participant.name, idle.baseAnimationId, idle.baseFrames.size(),
                         idle.baseFrames.empty() ? 0 : idle.baseFrames.front().size(),
                         participant.speechFrames.front().size());
                } else {
                    participant.idleFrames = std::move(idle.baseFrames);
                    participant.idleAnimationId = idle.baseAnimationId;
                    // Random phase so two creatures that drew the same idle
                    // animation don't loop in lockstep.
                    std::uniform_int_distribution<std::size_t> phaseDist(0, participant.idleFrames.size() - 1);
                    participant.idleStartOffset = phaseDist(rng);
                }
            }
        }

        participants_.push_back(std::move(participant));
    }

    // Look up universe for playback: every participant has to be registered
    // on the same one, because a turn is one animation on one universe.
    auto universeResult = creatures::resolveCommonUniverse(config_.creatureIds);
    if (!universeResult.isSuccess()) {
        return fail(universeResult.getError().value(), "CreatureUniverseMissing");
    }
    universe_ = universeResult.getValue().value();

    // Stage binding (#119). A dialog insists on one (#186): the birds have to
    // look at each other, so an unplaced participant is a configuration error
    // rather than a degrade.
    if (!config_.stageId.empty()) {
        auto stageResult = creatures::db->getStage(config_.stageId, startSpan);
        if (!stageResult.isSuccess()) {
            return fail(stageResult.getError().value(), "StageLookupFailed");
        }
        stage_ = stageResult.getValue().value();
        const auto placements = creatures::stagePlacements(stage_);
        gazeGeometries_.clear();
        gazeGeometries_.reserve(participants_.size());
        for (const auto &participant : participants_) {
            const auto placement = std::find_if(placements.begin(), placements.end(), [&](const auto &candidate) {
                return candidate.creature_id == participant.creatureId;
            });
            if (placement == placements.end()) {
                return fail(ServerError(ServerError::InvalidData,
                                        fmt::format("{} is not placed on stage '{}'", participant.name,
                                                    stage_.title.empty() ? config_.stageId : stage_.title)),
                            "CreatureNotOnStage");
            }
            gazeGeometries_.push_back(voice::resolveGazeGeometry(participant.creature, *placement));
        }
        // Per-creature rng streams, seeded off the shared generator so the
        // whole session stays reproducible from one seed but each creature
        // gets independent reaction timing (#119).
        gazeRngs_.clear();
        for (std::size_t i = 0; i < participants_.size(); ++i) {
            gazeRngs_.emplace_back(static_cast<uint32_t>(rng()));
        }
        haveStage_ = true;
    }

    // Load CMU dictionary
    auto cmuDictPath = creatures::config->getCmuDictPath();
    if (!cmuDictPath.empty()) {
        textToViseme_.loadCmuDict(cmuDictPath);
    }

    // Record the exchange right away (status "streaming") so the exchange list
    // can answer "what's being said right now" (issue #150). Best-effort: a
    // record-keeping failure must never block the creature from speaking.
    {
        creatures::AdHocExchange exchange;
        exchange.session_id = sessionId_;
        exchange.creature_id = participants_.front().creatureId;
        exchange.creature_name = participants_.front().name;
        exchange.status = EXCHANGE_STATUS_STREAMING;
        if (config_.dialog) {
            exchange.stage_id = config_.stageId;
            for (const auto &participant : participants_) {
                exchange.participants.push_back({participant.creatureId, participant.name, participant.audioChannel});
            }
        }
        auto publishResult = creatures::storage::publishAdHocExchange(exchange, startSpan);
        if (!publishResult.isSuccess()) {
            warn("Unable to record ad-hoc exchange {}: {}", sessionId_, publishResult.getError()->getMessage());
            if (startSpan) {
                startSpan->setAttribute("exchange.persistence.outcome", "failed");
                startSpan->setAttribute("exchange.persistence.error", publishResult.getError()->getMessage());
            }
        } else if (startSpan) {
            startSpan->setAttribute("exchange.persistence.outcome", "success");
        }
    }

    const auto &lead = participants_.front();
    info("StreamingAdHocSession started: session={}, participants={}, voice={}, model={}, base_anim={} ({} frames), "
         "stage={}",
         sessionId_, participants_.size(), lead.voiceId, lead.modelId, lead.speechAnimationId, lead.speechFrames.size(),
         haveStage_ ? stage_.id : "none");

    if (startSpan) {
        startSpan->setAttribute("voice.id", lead.voiceId);
        startSpan->setAttribute("voice.model", lead.modelId);
        startSpan->setAttribute("animation.base.id", lead.speechAnimationId);
        startSpan->setAttribute("animation.base.frames", static_cast<int64_t>(lead.speechFrames.size()));
        startSpan->setAttribute("session.participants", static_cast<int64_t>(participants_.size()));
        startSpan->setAttribute("playback.universe", static_cast<int64_t>(universe_));
        if (haveStage_)
            startSpan->setAttribute("stage.id", stage_.id);
        startSpan->setSuccess();
    }

    return Result<void>{};
}

Result<void> StreamingAdHocSession::addText(const std::string &text, std::shared_ptr<RequestSpan> triggerSpan) {
    if (participants_.size() != 1) {
        return Result<void>{
            ServerError(ServerError::Conflict, "This session has several participants; send turns with a creature_id")};
    }
    return addTurn(participants_.front().creatureId, text, std::move(triggerSpan));
}

Result<void> StreamingAdHocSession::addTurn(const std::string &creatureId, const std::string &text,
                                            std::shared_ptr<RequestSpan> triggerSpan) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (finished_.load()) {
        return Result<void>{ServerError(ServerError::Conflict, "Streaming session is already finishing")};
    }
    // Who is speaking. Accept either spelling of the id (Swift sends
    // uppercase), but keep the participant's stored spelling for everything.
    std::size_t speakerIndex = participants_.size();
    for (std::size_t i = 0; i < participants_.size(); ++i) {
        if (participants_[i].creatureId == creatureId ||
            (isUuidShape(creatureId) && canonicalUuid(participants_[i].creatureId) == canonicalUuid(creatureId))) {
            speakerIndex = i;
            break;
        }
    }
    if (speakerIndex == participants_.size()) {
        return Result<void>{ServerError(ServerError::InvalidData,
                                        fmt::format("Creature {} is not a participant in this session", creatureId))};
    }
    if (text.empty() || text.size() > MAX_STREAMING_AD_HOC_CHUNK_TEXT_BYTES) {
        return Result<void>{ServerError(ServerError::InvalidData,
                                        fmt::format("Streaming text chunk must contain between 1 and {} bytes",
                                                    MAX_STREAMING_AD_HOC_CHUNK_TEXT_BYTES))};
    }
    if (static_cast<std::size_t>(chunksReceived_.load()) >= MAX_STREAMING_AD_HOC_CHUNKS) {
        return Result<void>{
            ServerError(ServerError::Conflict,
                        fmt::format("Streaming session reached its {} chunk limit", MAX_STREAMING_AD_HOC_CHUNKS))};
    }
    const auto separatorBytes = fullText_.empty() ? 0U : 1U;
    if (text.size() + separatorBytes > MAX_STREAMING_AD_HOC_TOTAL_TEXT_BYTES - fullText_.size()) {
        return Result<void>{
            ServerError(ServerError::Conflict, fmt::format("Streaming session reached its {} byte transcript limit",
                                                           MAX_STREAMING_AD_HOC_TOTAL_TEXT_BYTES))};
    }
    if (outstandingSentenceWork_.load(std::memory_order_acquire) >= MAX_PENDING_STREAMING_AD_HOC_SENTENCES) {
        if (triggerSpan) {
            triggerSpan->setAttribute("admission.outcome", "rejected");
            triggerSpan->setAttribute("admission.scope", "session");
            triggerSpan->setAttribute("admission.limit", static_cast<int64_t>(MAX_PENDING_STREAMING_AD_HOC_SENTENCES));
        }
        return Result<void>{
            ServerError(ServerError::Conflict, fmt::format("Streaming session already has {} pending sentences",
                                                           MAX_PENDING_STREAMING_AD_HOC_SENTENCES))};
    }
    auto renderReservation = tryReserveGlobalRender();
    if (!renderReservation) {
        if (triggerSpan) {
            triggerSpan->setAttribute("admission.outcome", "rejected");
            triggerSpan->setAttribute("admission.scope", "global");
            triggerSpan->setAttribute("admission.limit", static_cast<int64_t>(MAX_GLOBAL_STREAMING_AD_HOC_RENDERS));
        }
        return Result<void>{
            ServerError(ServerError::Conflict, fmt::format("Streaming speech already has {} queued or active renders",
                                                           MAX_GLOBAL_STREAMING_AD_HOC_RENDERS))};
    }
    if (triggerSpan) {
        triggerSpan->setAttribute("admission.outcome", "accepted");
        triggerSpan->setAttribute("admission.session.pending",
                                  static_cast<int64_t>(outstandingSentenceWork_.load(std::memory_order_relaxed) + 1));
        triggerSpan->setAttribute("admission.global.reserved",
                                  static_cast<int64_t>(globalReservedRenders.load(std::memory_order_relaxed)));
    }
    touchClientActivity();
    if (!fullText_.empty()) {
        fullText_ += " ";
    }
    fullText_ += text;
    const int sentenceIndex = chunksReceived_.fetch_add(1) + 1;
    outstandingSentenceWork_.fetch_add(1, std::memory_order_release);
    turns_.push_back({speakerIndex, text});

    const auto &speaker = participants_[speakerIndex];
    info("StreamingAdHocSession received sentence {} from {} ({} bytes)", sentenceIndex, speaker.name, text.size());

    // Create the promise/future pair this turn resolves for the next one: the
    // per-creature continuity snapshot (body-loop phase, idle phase, prosody
    // request id, head aiming) every later turn builds on.
    {
        std::lock_guard<std::mutex> lock(offsetMutex_);
        continuityPromises_.emplace_back();
        continuityFutures_.push_back(continuityPromises_.back().get_future().share());
    }

    // Kick off full pipeline (TTS + WAV wrap + Opus + animation build) in background.
    auto sentenceSpan = creatures::observability ? creatures::observability->createLinkedOperationSpan(
                                                       "StreamingAdHocSession.sentence", std::move(triggerSpan))
                                                 : nullptr;
    if (sentenceSpan) {
        sentenceSpan->setAttribute("session.id", sessionId_);
        sentenceSpan->setAttribute("creature.id", speaker.creatureId);
        sentenceSpan->setAttribute("sentence.index", static_cast<int64_t>(sentenceIndex));
        sentenceSpan->setAttribute("sentence.length", static_cast<int64_t>(text.size()));
    }

    std::packaged_task<Result<RenderedTurn>()> renderTask([this, text, sentenceIndex, speakerIndex, sentenceSpan,
                                                           renderReservation =
                                                               std::move(renderReservation)]() -> Result<RenderedTurn> {
        (void)renderReservation;
        // Whatever this turn received is what a failure forwards, so the
        // chain never stalls on a turn that rendered nothing.
        TurnContinuity previous = initialContinuity();
        try {
            const Participant &speaker = participants_[speakerIndex];

            // 0. Continuity from the previous turn. Read the shared_future
            // under the lock — concurrent addTurn() can be doing push_back()
            // which would invalidate an iterator-style access; copying the
            // shared_future locally is safe because shared_future is itself
            // reference-counted.
            if (sentenceIndex > 1) {
                std::shared_future<TurnContinuity> previousFuture;
                {
                    std::lock_guard<std::mutex> lock(offsetMutex_);
                    previousFuture = continuityFutures_[sentenceIndex - 2];
                }
                previous = previousFuture.get();
            }
            const CreatureContinuity &speakerBefore = previous.creatures[speakerIndex];

            // 1. TTS via REST with previous_request_ids for prosody continuity —
            // this voice's own last request, not whoever spoke last.
            std::vector<std::string> prevIds;
            if (!speakerBefore.lastRequestId.empty()) {
                prevIds.push_back(speakerBefore.lastRequestId);
            }

            StreamingTTSClient client;
            // Request raw mono 48 kHz S16 PCM directly (issue #12). The
            // 17-channel WAV is wrapped in-process below; no ffmpeg decode hop.
            auto ttsResult = client.generateSpeechREST(creatures::config->getVoiceApiKey(), speaker.voiceId,
                                                       speaker.modelId, text, "pcm_48000", speaker.stability,
                                                       speaker.similarityBoost, prevIds, nullptr, sentenceSpan);
            if (!ttsResult.isSuccess()) {
                const auto failure = ttsResult.getError().value();
                lifecycleFailed_.store(true);
                recordSpanError(sentenceSpan, failure.getMessage(), "TextToSpeechFailed", failure.getCode());
                resolveFailedSentence(sentenceIndex, previous);
                return Result<RenderedTurn>{ttsResult.getError().value()};
            }
            const auto tts = ttsResult.getValue().value();

            // 2. Wrap raw PCM into a 17-channel WAV (in-process; previously
            // ffmpeg via AudioConverter::convertMp3ToWav). See issue #12.
            auto tempDir = std::filesystem::temp_directory_path() / "creature-adhoc" / sessionId_;
            std::filesystem::create_directories(tempDir);

            auto wavPath = tempDir / fmt::format("s{}.wav", sentenceIndex);
            {
                // Scoped so the span measures the write alone; it used to
                // stay open across the encode and report both as one number.
                auto pcmSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                              "StreamingAdHocSession.wrapPcm", sentenceSpan)
                                                        : nullptr;
                if (pcmSpan) {
                    pcmSpan->setAttribute("audio.input.bytes", static_cast<int64_t>(tts.audioData.size()));
                    pcmSpan->setAttribute("audio.channel", static_cast<int64_t>(speaker.audioChannel));
                    pcmSpan->setAttribute("audio.sample.rate", static_cast<int64_t>(48000));
                }
                auto convertResult = writePcmToMultichannelWav(tts.audioData, wavPath, speaker.audioChannel, 48000);
                if (!convertResult.isSuccess()) {
                    const auto failure = convertResult.getError().value();
                    lifecycleFailed_.store(true);
                    recordSpanError(pcmSpan, failure.getMessage(), "PcmWavWriteFailed", failure.getCode());
                    recordSpanError(sentenceSpan, failure.getMessage(), "PcmWavWriteFailed", failure.getCode());
                    resolveFailedSentence(sentenceIndex, previous);
                    return Result<RenderedTurn>{convertResult.getError().value()};
                }
                if (pcmSpan)
                    pcmSpan->setSuccess();
            }

            // 3. Opus encoding, straight from the PCM still in hand — no
            // re-reading the 17-channel file we just wrote, and the silent
            // lanes encoded once (issue #195). Never charged to the retention
            // budget that keeps show audio warm (issue #93): on the mainstage
            // the session itself holds the buffer until the turn has played,
            // so playback finds it in the memo and the disk cache is skipped
            // (issue #197); in travel mode the buffer is discarded here and
            // playback loads it from the disk cache, exactly as before.
            const bool holdInMemory = creatures::config && !creatures::config->getTravelMode();
            auto audioBuffer = creatures::rtp::AudioStreamBuffer::loadFromMonoPcm(
                wavPath.string(),
                std::span<const int16_t>(reinterpret_cast<const int16_t *>(tts.audioData.data()),
                                         tts.audioData.size() / 2),
                speaker.audioChannel, sentenceSpan, creatures::rtp::AudioStreamBuffer::RetentionIntent::OneShot,
                holdInMemory ? creatures::rtp::AudioStreamBuffer::DiskCache::Skip
                             : creatures::rtp::AudioStreamBuffer::DiskCache::Publish);
            if (!holdInMemory) {
                audioBuffer.reset();
            }
            if (sentenceSpan) {
                sentenceSpan->setAttribute("audio.held_in_memory", holdInMemory && audioBuffer != nullptr);
            }

            // 4. Build animation frames
            size_t targetFrames = std::max<size_t>(
                1,
                static_cast<size_t>(std::ceil((tts.audioDurationSeconds * 1000.0) / static_cast<double>(msPerFrame_))));

            // Convert the provider's character alignment into mouth frames.
            // Keep this as one coarse span: per-cue/per-frame spans would add
            // volume without making the pipeline easier to query.
            auto lipSyncSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                              "StreamingAdHocSession.buildLipSync", sentenceSpan)
                                                        : nullptr;
            if (lipSyncSpan) {
                lipSyncSpan->setAttribute("alignment.characters", static_cast<int64_t>(tts.charTimings.size()));
                lipSyncSpan->setAttribute("animation.target.frames", static_cast<int64_t>(targetFrames));
            }
            std::vector<uint8_t> mouthData;
            try {
                std::vector<RhubarbMouthCue> mouthCues;
                if (!tts.charTimings.empty()) {
                    mouthCues = textToViseme_.charTimingsToMouthCues(tts.charTimings);
                }
                RhubarbSoundData lipSyncData;
                lipSyncData.metadata.soundFile = wavPath.filename().string();
                lipSyncData.metadata.duration = tts.audioDurationSeconds;
                lipSyncData.mouthCues = mouthCues;

                SoundDataProcessor processor;
                mouthData = processor.processSoundData(lipSyncData, msPerFrame_, targetFrames);
                if (lipSyncSpan) {
                    lipSyncSpan->setAttribute("mouth.cues", static_cast<int64_t>(mouthCues.size()));
                    lipSyncSpan->setAttribute("mouth.frames", static_cast<int64_t>(mouthData.size()));
                    lipSyncSpan->setSuccess();
                }
            } catch (const std::exception &exception) {
                if (lipSyncSpan)
                    lipSyncSpan->recordException(exception);
                recordSpanError(lipSyncSpan, exception.what(), "LipSyncBuildException", ServerError::InternalError);
                throw;
            }

            // 5. One track per participant (issue #186). The speaker cycles
            // its speech loop with the mouth driven; everyone else cycles its
            // idle loop with its beak shut. Each creature picks up where the
            // previous turn left it, and eases across when it changes role.
            // With a stage bound, the speaker plays to the house and the
            // listeners turn to the speaker.
            const std::string animationId = util::generateUUID();
            TurnContinuity next;
            next.creatures.resize(participants_.size());
            std::vector<Track> tracks;
            tracks.reserve(participants_.size());
            const std::vector<SpeakerSpan> timeline{SpeakerSpan{0, targetFrames, speaker.creatureId}};
            auto trackSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                            "StreamingAdHocSession.buildTrack", sentenceSpan)
                                                      : nullptr;
            if (trackSpan) {
                trackSpan->setAttribute("animation.target.frames", static_cast<int64_t>(targetFrames));
                trackSpan->setAttribute("animation.tracks", static_cast<int64_t>(participants_.size()));
            }
            for (std::size_t i = 0; i < participants_.size(); ++i) {
                const Participant &participant = participants_[i];
                const CreatureContinuity &before = previous.creatures[i];
                const bool isSpeaker = i == speakerIndex;

                // Shared frame-build via the speech track builder (issue #15).
                // mouth_slot bounds check + body cycle + mouth-byte insertion
                // all live in one place.
                SpeechTrackInput trackInput;
                trackInput.baseFrames = participant.speechFrames;
                if (isSpeaker)
                    trackInput.mouthBytes = mouthData;
                trackInput.mouthSlot = participant.mouthSlot;
                trackInput.totalFrames = targetFrames;
                trackInput.creatureId = participant.creatureId;
                trackInput.animationId = animationId;

                SpeechTrackOptions trackOptions;
                if (isSpeaker) {
                    trackOptions.startOffset = before.speechOffset;
                } else {
                    trackOptions.dialogIdleMode = true;
                    trackOptions.idleFrames = participant.idleFrames;
                    trackOptions.idleStartOffset = before.idleOffset;
                }
                // A creature that changed role since the last turn is on a
                // different loop now; ease in from where its body was.
                if (before.primed && before.wasSpeaking != isSpeaker) {
                    trackOptions.entryFadeFrom = before.lastBodyFrame;
                }

                GazeContinuity gaze = before.gaze;
                GazeTrack gazeTrack;
                if (haveStage_) {
                    gazeTrack = buildGazeTrack(gazeGeometries_[i], gazeGeometries_, timeline, targetFrames, msPerFrame_,
                                               gazeRngs_[i], GazeOptions{}, &gaze);
                    trackInput.gazePanBytes = gazeTrack.panBytes;
                    trackInput.gazeElevationBytes = gazeTrack.elevationBytes;
                    trackInput.gazeCockBytes = gazeTrack.cockBytes;
                    trackInput.gazePanSlot = gazeTrack.panSlot;
                    trackInput.gazeElevationSlot = gazeTrack.elevationSlot;
                    trackInput.gazeCockSlot = gazeTrack.cockSlot;
                }

                auto trackResult = buildSpeechTrack(trackInput, trackOptions, trackSpan);
                if (!trackResult.isSuccess()) {
                    const auto failure = trackResult.getError().value();
                    lifecycleFailed_.store(true);
                    recordSpanError(trackSpan, failure.getMessage(), "SpeechTrackBuildFailed", failure.getCode());
                    recordSpanError(sentenceSpan, failure.getMessage(), "SpeechTrackBuildFailed", failure.getCode());
                    resolveFailedSentence(sentenceIndex, previous);
                    return Result<RenderedTurn>{trackResult.getError().value()};
                }
                auto built = trackResult.getValue().value();

                CreatureContinuity &after = next.creatures[i];
                after.primed = true;
                after.wasSpeaking = isSpeaker;
                after.speechOffset = isSpeaker ? built.endOffset : before.speechOffset;
                after.idleOffset = (!isSpeaker && built.idleFramesUsed) ? built.idleEndOffset : before.idleOffset;
                after.lastBodyFrame = std::move(built.lastBodyFrame);
                after.lastRequestId = isSpeaker ? tts.requestId : before.lastRequestId;
                after.gaze = gaze;

                tracks.push_back(std::move(built.track));
            }
            if (trackSpan) {
                trackSpan->setAttribute("animation.frames", static_cast<int64_t>(targetFrames));
                trackSpan->setAttribute("frame.base.offset",
                                        static_cast<int64_t>(previous.creatures[speakerIndex].speechOffset));
                trackSpan->setAttribute("frame.end.offset",
                                        static_cast<int64_t>(next.creatures[speakerIndex].speechOffset));
                trackSpan->setSuccess();
            }

            // 6. Hand the next turn its starting point.
            {
                std::lock_guard<std::mutex> lock(offsetMutex_);
                continuityPromises_[sentenceIndex - 1].set_value(next);
            }

            // 7. Build animation object
            auto textSlug = util::slugify(tts.alignmentText.empty() ? text : tts.alignmentText, 40, "speech");
            Animation animation = participants_.front().baseAnimation;
            animation.id = animationId;
            animation.metadata.animation_id = animation.id;
            // Named after what it is (#126), matching the exchange title shape;
            // the ad-hoc list already shows created_at.
            animation.metadata.title = fmt::format("{} - s{} - {}", speaker.name, sentenceIndex, textSlug);
            animation.metadata.sound_file = wavPath.string();
            animation.metadata.note = fmt::format("Streaming sentence {}: {}", sentenceIndex, text);
            animation.metadata.number_of_frames = static_cast<uint32_t>(targetFrames);
            animation.metadata.multitrack_audio = true;
            animation.tracks = std::move(tracks);

            // Expiration can race the one already-running render after queued
            // work is discarded. Do not persist or dispatch that stale result.
            if (cancelled_.load(std::memory_order_acquire)) {
                lifecycleFailed_.store(true);
                recordSpanError(sentenceSpan, "Streaming render cancelled after session expiry", "RenderCancelled",
                                ServerError::Conflict);
                return Result<RenderedTurn>{ServerError(ServerError::Conflict, "Streaming render cancelled")};
            }

            // 8. Insert into DB. Storage facade pairs the insert + invalidations
            // so each sentence's clients learn about the new artifact ASAP
            // (issue #11).
            auto publishResult = creatures::storage::publishAdHocAnimation(animation, sentenceSpan);
            if (!publishResult.isSuccess()) {
                warn("Unable to publish streaming sentence {} animation: {}", sentenceIndex,
                     publishResult.getError()->getMessage());
            }

            if (sentenceSpan) {
                sentenceSpan->setAttribute("animation.id", animation.id);
                sentenceSpan->setAttribute("animation.frames", static_cast<int64_t>(targetFrames));
                sentenceSpan->setAttribute("frame.base.offset",
                                           static_cast<int64_t>(previous.creatures[speakerIndex].speechOffset));
                sentenceSpan->setSuccess();
            }

            info("Sentence {} animation ready: {} frames, {} tracks, {:.2f}s", sentenceIndex, targetFrames,
                 animation.tracks.size(), tts.audioDurationSeconds);

            RenderedTurn rendered;
            rendered.animation = std::move(animation);
            // S16 mono: two bytes per sample.
            rendered.audioSamples = static_cast<uint64_t>(tts.audioData.size() / 2);
            rendered.requestId = tts.requestId;
            rendered.audioBuffer = std::move(audioBuffer);
            return rendered;
        } catch (const std::exception &exception) {
            lifecycleFailed_.store(true);
            if (sentenceSpan)
                sentenceSpan->recordException(exception);
            recordSpanError(sentenceSpan, exception.what(), "BackgroundPipelineException", ServerError::InternalError);
            resolveFailedSentence(sentenceIndex, previous);
            return Result<RenderedTurn>{ServerError(ServerError::InternalError, "Background speech pipeline failed")};
        } catch (...) {
            lifecycleFailed_.store(true);
            recordSpanError(sentenceSpan, "Unknown background speech pipeline failure",
                            "UnknownBackgroundPipelineException", ServerError::InternalError);
            resolveFailedSentence(sentenceIndex, previous);
            return Result<RenderedTurn>{ServerError(ServerError::InternalError, "Background speech pipeline failed")};
        }
    });
    auto future = renderTask.get_future();

    {
        std::lock_guard<std::mutex> lock(futuresMutex_);
        sentenceFutures_.push_back(std::move(future));
        sentenceSpans_.push_back(sentenceSpan);
    }
    {
        std::lock_guard<std::mutex> lock(renderMutex_);
        renderTasks_.push_back(std::move(renderTask));
    }

    // Spawn the playback thread on the first sentence. It will start waiting
    // for sentence 1's future to resolve and trigger playback immediately.
    if (sentenceIndex == 1) {
        renderThread_ = std::thread(&StreamingAdHocSession::renderThreadFunc, this);
        playbackThread_ = std::thread(&StreamingAdHocSession::playbackThreadFunc, this);
    }

    renderCv_.notify_one();
    // Wake the playback thread so it knows a new future is available.
    // The future is already in the vector (pushed under lock above), so the
    // playback thread's predicate will see it when it re-checks.
    playbackCv_.notify_one();

    debug("Sentence {} queued for pipelined playback", sentenceIndex);

    return Result<void>{};
}

void StreamingAdHocSession::renderThreadFunc() {
    info("Render thread started for session {}", sessionId_);
    while (true) {
        std::packaged_task<Result<RenderedTurn>()> task;
        {
            std::unique_lock<std::mutex> lock(renderMutex_);
            renderCv_.wait(lock, [&] { return !renderTasks_.empty() || finished_.load() || cancelled_.load(); });
            if (cancelled_.load()) {
                renderTasks_.clear();
                break;
            }
            if (renderTasks_.empty()) {
                if (finished_.load())
                    break;
                continue;
            }
            task = std::move(renderTasks_.front());
            renderTasks_.pop_front();
        }
        task();
    }
    info("Render thread finished for session {}", sessionId_);
}

void StreamingAdHocSession::playbackThreadFunc() {
    info("Playback thread started for session {}", sessionId_);

    size_t nextIndex = 0;
    std::string lastAnimationId;

    while (true) {
        // Wait until there's a future to process or we're told to stop
        std::unique_lock<std::mutex> lock(futuresMutex_);
        playbackCv_.wait(lock, [&] { return nextIndex < sentenceFutures_.size() || finished_.load(); });

        // Process all available futures in order
        while (nextIndex < sentenceFutures_.size()) {
            // Move the future out so we can release the lock while waiting on it
            auto future = std::move(sentenceFutures_[nextIndex]);
            auto sentenceSpan = sentenceSpans_[nextIndex];
            lock.unlock();

            int sentenceIndex = static_cast<int>(nextIndex + 1);

            std::optional<Result<RenderedTurn>> animationResult;
            try {
                animationResult.emplace(future.get());
            } catch (const std::exception &exception) {
                lifecycleFailed_.store(true);
                error("Sentence {} background pipeline threw: {}", sentenceIndex, exception.what());
                if (sentenceSpan)
                    sentenceSpan->recordException(exception);
                recordSpanError(
                    sentenceSpan, exception.what(),
                    cancelled_.load(std::memory_order_acquire) ? "RenderCancelled" : "BackgroundPipelineException",
                    cancelled_.load(std::memory_order_acquire) ? ServerError::Conflict : ServerError::InternalError);
                resolveFailedSentence(sentenceIndex, continuityBefore(sentenceIndex));
            } catch (...) {
                lifecycleFailed_.store(true);
                error("Sentence {} background pipeline threw an unknown exception", sentenceIndex);
                recordSpanError(sentenceSpan, "Unknown background pipeline exception",
                                cancelled_.load(std::memory_order_acquire) ? "RenderCancelled"
                                                                           : "UnknownBackgroundPipelineException",
                                cancelled_.load(std::memory_order_acquire) ? ServerError::Conflict
                                                                           : ServerError::InternalError);
                resolveFailedSentence(sentenceIndex, continuityBefore(sentenceIndex));
            }
            if (!animationResult || !animationResult->isSuccess()) {
                if (animationResult) {
                    warn("Sentence {} failed: {}", sentenceIndex, animationResult->getError()->getMessage());
                }
                lock.lock();
                sentenceOutcomes_.push_back({});
                outstandingSentenceWork_.fetch_sub(1, std::memory_order_release);
                nextIndex++;
                continue;
            }
            auto rendered = animationResult->getValue().value();
            auto &animation = rendered.animation;
            if (cancelled_.load(std::memory_order_acquire)) {
                lifecycleFailed_.store(true);
                recordSpanError(sentenceSpan, "Playback suppressed after streaming session expiry", "RenderCancelled",
                                ServerError::Conflict);
                lock.lock();
                sentenceOutcomes_.push_back({});
                outstandingSentenceWork_.fetch_sub(1, std::memory_order_release);
                nextIndex++;
                continue;
            }
            auto playbackSpan =
                creatures::observability
                    ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.playback", sentenceSpan)
                    : nullptr;
            if (playbackSpan) {
                playbackSpan->setAttribute("session.id", sessionId_);
                playbackSpan->setAttribute("creature.id", participants_.front().creatureId);
                playbackSpan->setAttribute("sentence.index", static_cast<int64_t>(sentenceIndex));
                playbackSpan->setAttribute("animation.id", animation.id);
                playbackSpan->setAttribute("playback.universe", static_cast<int64_t>(universe_));
            }

            bool dispatched = true;
            // Serialize the terminal cancellation check with physical
            // dispatch. Expiry can either win and suppress this sentence, or
            // wait until a valid dispatch is fully registered; it cannot slip
            // between the check and interrupt/queue/schedule.
            {
                std::lock_guard<std::mutex> dispatchLock(stateMutex_);
                if (cancelled_.load(std::memory_order_acquire)) {
                    if (playbackSpan)
                        playbackSpan->setAttribute("playback.dispatch.mode", "suppressed");
                    recordSpanError(playbackSpan, "Playback suppressed after streaming session expiry",
                                    "RenderCancelled", ServerError::Conflict);
                    recordSpanError(sentenceSpan, "Playback suppressed after streaming session expiry",
                                    "RenderCancelled", ServerError::Conflict);
                    dispatched = false;
                } else if (nextIndex == 0) {
                    if (playbackSpan)
                        playbackSpan->setAttribute("playback.dispatch.mode", "interrupt");
                    info("Sentence {}: interrupt() for immediate playback (pipelined!)", sentenceIndex);
                    // Our session id is the chain id: every sentence's playback session
                    // carries it, so queue entries and failure cleanup stay scoped to
                    // this chain (issue #100).
                    auto sessionResult = creatures::sessionManager->interrupt(
                        universe_, animation, config_.resumePlaylist, nullptr, sessionId_);
                    if (!sessionResult.isSuccess()) {
                        warn("Sentence {} playback failed: {}", sentenceIndex, sessionResult.getError()->getMessage());
                        const auto failure = sessionResult.getError().value();
                        recordSpanError(playbackSpan, failure.getMessage(), "PlaybackInterruptFailed",
                                        failure.getCode());
                        dispatched = false;
                    }
                } else {
                    if (playbackSpan)
                        playbackSpan->setAttribute("playback.dispatch.mode", "queue");
                    info("Sentence {}: queueAnimation() for chained playback", sentenceIndex);
                    const bool queued = creatures::sessionManager->queueAnimation(universe_, animation, sessionId_);
                    if (!queued) {
                        // The chain went quiet — a short earlier sentence finished
                        // before this render resolved. Play the sentence now instead
                        // of stranding an entry no session could ever pop (issue #100).
                        info("Sentence {}: chain idle, scheduling directly", sentenceIndex);
                        if (playbackSpan)
                            playbackSpan->setAttribute("playback.dispatch.mode", "direct");
                        auto scheduled = creatures::CooperativeAnimationScheduler::scheduleAnimation(
                            creatures::eventLoop ? creatures::eventLoop->getNextFrameNumber() : 0, animation, universe_,
                            creatures::runtime::ActivityReason::AdHoc, false, sessionId_);
                        if (!scheduled.isSuccess()) {
                            warn("Sentence {} direct playback failed: {}", sentenceIndex,
                                 scheduled.getError()->getMessage());
                            const auto failure = scheduled.getError().value();
                            recordSpanError(playbackSpan, failure.getMessage(), "DirectPlaybackScheduleFailed",
                                            failure.getCode());
                            dispatched = false;
                        }
                    }
                }
            }

            if (playbackSpan) {
                playbackSpan->setAttribute("playback.dispatched", dispatched);
                if (dispatched)
                    playbackSpan->setSuccess();
            }
            if (!dispatched)
                lifecycleFailed_.store(true);
            else
                lastAnimationId = animation.id;

            // The turn is on its way; now the encoded frames can go to the
            // disk cache without anyone waiting on them (issue #197).
            if (rendered.audioBuffer) {
                publishAudioCacheInBackground(rendered.audioBuffer, animation.metadata.sound_file, sentenceSpan);
            }

            SentenceOutcome outcome;
            outcome.success = dispatched;
            if (dispatched) {
                outcome.animationId = animation.id;
                outcome.requestId = rendered.requestId;
                outcome.audioSamples = rendered.audioSamples;
                outcome.audioBuffer = std::move(rendered.audioBuffer);
                // The stitched exchange animation (#186) needs every turn's
                // frames; a plain ad-hoc stream doesn't build one, so don't
                // hold a copy of every sentence for it.
                if (config_.dialog) {
                    outcome.tracks = std::move(animation.tracks);
                }
            }
            lock.lock();
            sentenceOutcomes_.push_back(std::move(outcome));
            outstandingSentenceWork_.fetch_sub(1, std::memory_order_release);
            nextIndex++;
        }

        // If finish() has been called and we've processed everything, we're done
        if (finished_.load() && nextIndex >= sentenceFutures_.size()) {
            break;
        }
    }

    info("Playback thread finished for session {} (last animation: {})", sessionId_, lastAnimationId);
}

Result<StreamingFinishResult> StreamingAdHocSession::finish(std::shared_ptr<RequestSpan> triggerSpan) {
    auto finishSpan =
        creatures::observability
            ? creatures::observability->createOperationSpan("StreamingAdHocSession.finish", std::move(triggerSpan))
            : nullptr;

    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        touchClientActivity();
        if (fullText_.empty()) {
            recordSpanError(finishSpan, "No text was added to the session", "EmptyStreamingSession",
                            ServerError::InvalidData);
            return Result<StreamingFinishResult>{
                ServerError(ServerError::InvalidData, "No text was added to the session")};
        }
        if (finished_.exchange(true)) {
            recordSpanError(finishSpan, "Streaming session is already finishing", "SessionAlreadyFinishing",
                            ServerError::Conflict);
            return Result<StreamingFinishResult>{
                ServerError(ServerError::Conflict, "Streaming session is already finishing")};
        }
    }

    const int chunksReceived = chunksReceived_.load();

    if (finishSpan) {
        finishSpan->setAttribute("text.length", static_cast<int64_t>(fullText_.size()));
        finishSpan->setAttribute("text.sentences", static_cast<int64_t>(chunksReceived));
    }

    info("StreamingAdHocSession finishing: session={}, {} sentences, signaling playback thread...", sessionId_,
         chunksReceived);

    // The transcript. A lone creature's is the words, exactly as before; a
    // dialog's names who said what, line by line, because that is what the
    // exchange list and the ID3 lyrics should show for a scene.
    std::string transcript;
    if (config_.dialog) {
        for (const auto &turn : turns_) {
            if (!transcript.empty())
                transcript += "\n";
            transcript += fmt::format("{}: {}", participants_[turn.speakerIndex].name, turn.text);
        }
    } else {
        transcript = fullText_;
    }

    // Write transcript
    auto tempDir = std::filesystem::temp_directory_path() / "creature-adhoc" / sessionId_;
    std::filesystem::create_directories(tempDir);
    {
        std::ofstream f(tempDir / "transcript.txt");
        f << transcript;
    }

    // Signal the playback thread that no more sentences are coming. The flag
    // was set under stateMutex_ above so a concurrent /text cannot slip in.
    renderCv_.notify_one();
    playbackCv_.notify_one();

    // Drain the bounded render queue first, then wait for playback to consume
    // every now-ready future.
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }
    joinCachePublishes();

    // No invalidations fired here — each sentence's publishAdHocAnimation above
    // already invalidates AdHocAnimationList + AdHocSoundList as the chunk lands.

    // The playback thread is joined, so every sentence's WAV that will ever
    // exist is on disk now — harvest the outcomes and stitch the exchange
    // (issue #150). No lock needed: nothing else touches these vectors anymore.
    std::vector<AdHocExchangePart> parts;
    std::vector<std::filesystem::path> partWavs;
    std::vector<TurnTrackPart> turnParts;
    std::vector<std::string> generationIds;
    std::string lastAnimationId;
    for (size_t i = 0; i < sentenceOutcomes_.size(); i++) {
        auto &outcome = sentenceOutcomes_[i];
        if (!outcome.success) {
            continue;
        }
        AdHocExchangePart part;
        part.index = static_cast<uint32_t>(i + 1);
        part.animation_id = outcome.animationId;
        part.text = i < turns_.size() ? turns_[i].text : "";
        if (config_.dialog && i < turns_.size()) {
            const auto &speaker = participants_[turns_[i].speakerIndex];
            part.creature_id = speaker.creatureId;
            part.creature_name = speaker.name;
        }
        parts.push_back(std::move(part));
        partWavs.push_back(tempDir / fmt::format("s{}.wav", i + 1));
        if (config_.dialog) {
            turnParts.push_back({std::move(outcome.tracks), outcome.audioSamples});
        }
        if (!outcome.requestId.empty()) {
            generationIds.push_back(outcome.requestId);
        }
        lastAnimationId = outcome.animationId;
    }

    // The cast, for the title and the provenance.
    std::string castName;
    for (const auto &participant : participants_) {
        if (!castName.empty())
            castName += " & ";
        castName += participant.name;
    }

    creatures::AdHocExchange exchange;
    exchange.session_id = sessionId_;
    exchange.creature_id = participants_.front().creatureId;
    exchange.creature_name = participants_.front().name;
    exchange.transcript = transcript;
    exchange.finished_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (config_.dialog) {
        exchange.stage_id = config_.stageId;
        for (const auto &participant : participants_) {
            exchange.participants.push_back({participant.creatureId, participant.name, participant.audioChannel});
        }
    }
    // Name the exchange after what it IS (#126), like dialog renders: creature
    // plus the words — with their natural punctuation, because this title
    // lands in the ID3 tags (#157). The download filename slugifies it, so
    // the dashes still show up exactly where they belong. No timestamp —
    // created_at rides in the record, and the filename gets a session-id
    // tail for uniqueness.
    exchange.title = fmt::format("{} - {}", castName, util::titleExcerpt(fullText_, 60, "exchange"));

    std::string exchangeAnimationId;
    if (parts.empty()) {
        exchange.status = EXCHANGE_STATUS_FAILED;
    } else {
        // The stitched WAV is the first ad-hoc artifact with real provenance:
        // the #148 tag mapping turns this into TITLE/ARTIST/LYRICS on the MP3.
        WavProvenance provenance;
        provenance.fileUid = sessionId_;
        provenance.take = "exchange";
        provenance.title = exchange.title;
        for (const auto &participant : participants_) {
            provenance.tracks.push_back({participant.audioChannel, participant.name});
        }
        for (const auto &part : parts) {
            const auto &speakerName = part.creature_name.empty() ? participants_.front().name : part.creature_name;
            provenance.script.push_back({speakerName, part.text});
        }
        provenance.generationIds = generationIds;

        const auto stitchedPath = tempDir / fmt::format("{}.wav", sessionId_);
        auto stitchSpan =
            creatures::observability
                ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.stitchExchange", finishSpan)
                : nullptr;
        if (stitchSpan) {
            stitchSpan->setAttribute("session.id", sessionId_);
            stitchSpan->setAttribute("exchange.parts", static_cast<int64_t>(partWavs.size()));
            stitchSpan->setAttribute("provenance.generation.ids",
                                     static_cast<int64_t>(provenance.generationIds.size()));
        }
        auto stitchResult = stitchMultichannelWavs(partWavs, stitchedPath, provenance);
        if (!stitchResult.isSuccess()) {
            warn("Failed to stitch exchange WAV for session {}: {}", sessionId_, stitchResult.getError()->getMessage());
            const auto failure = stitchResult.getError().value();
            recordSpanError(stitchSpan, failure.getMessage(), "ExchangeStitchFailed", failure.getCode());
            exchange.status = EXCHANGE_STATUS_FAILED;
        } else {
            // Result::getValue() returns the optional BY VALUE — copy, never
            // bind a reference through it (it dangles).
            const auto stitched = stitchResult.getValue().value();
            exchange.sound_file = stitchedPath.string();
            exchange.duration_ms = stitched.totalDurationMs;
            if (stitchSpan) {
                stitchSpan->setAttribute("audio.duration.ms", stitched.totalDurationMs);
                stitchSpan->setSuccess();
            }
            for (size_t i = 0; i < parts.size() && i < stitched.partDurationsMs.size(); i++) {
                parts[i].duration_ms = stitched.partDurationsMs[i];
            }
            exchange.status =
                static_cast<int>(parts.size()) == chunksReceived ? EXCHANGE_STATUS_READY : EXCHANGE_STATUS_PARTIAL;

            // The whole exchange as one N-track ad-hoc animation (issue #186),
            // sized against the stitched audio so lip sync stays aligned to
            // the last word. Best-effort: the turns already played and the
            // exchange record stands whether or not this lands.
            if (config_.dialog) {
                auto animationSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                    "StreamingAdHocSession.stitchAnimation", finishSpan)
                                                              : nullptr;
                const std::string stitchedAnimationId = util::generateUUID();
                auto concatResult = concatTurnTracks(turnParts, msPerFrame_, 48000, stitchedAnimationId);
                if (!concatResult.isSuccess()) {
                    warn("Unable to stitch exchange animation for session {}: {}", sessionId_,
                         concatResult.getError()->getMessage());
                    const auto failure = concatResult.getError().value();
                    recordSpanError(animationSpan, failure.getMessage(), "ExchangeAnimationConcatFailed",
                                    failure.getCode());
                } else {
                    auto concatenated = concatResult.getValue().value();
                    Animation animation = participants_.front().baseAnimation;
                    animation.id = stitchedAnimationId;
                    animation.metadata.animation_id = animation.id;
                    animation.metadata.title = exchange.title;
                    animation.metadata.sound_file = stitchedPath.string();
                    animation.metadata.note = transcript;
                    animation.metadata.number_of_frames = static_cast<uint32_t>(concatenated.totalFrames);
                    animation.metadata.milliseconds_per_frame = msPerFrame_;
                    animation.metadata.multitrack_audio = true;
                    // Same provenance the dialog job stamps (#119), so "is
                    // this render stale against its stage?" is one comparison.
                    animation.metadata.render_seed = renderSeed_;
                    animation.metadata.source_render_choices.clear();
                    for (const auto &participant : participants_) {
                        creatures::CreatureRenderChoice choice;
                        choice.creature_id = participant.creatureId;
                        choice.speech_loop_animation_id = participant.speechAnimationId;
                        choice.idle_animation_id = participant.idleAnimationId;
                        choice.idle_start_offset = static_cast<uint32_t>(participant.idleStartOffset);
                        animation.metadata.source_render_choices.push_back(std::move(choice));
                    }
                    if (haveStage_) {
                        animation.metadata.source_stage_id = stage_.id;
                        animation.metadata.source_stage_updated_at = stage_.updated_at;
                    }
                    animation.tracks = std::move(concatenated.tracks);

                    auto publishResult = creatures::storage::publishAdHocAnimation(animation, animationSpan);
                    if (!publishResult.isSuccess()) {
                        warn("Unable to publish exchange animation for session {}: {}", sessionId_,
                             publishResult.getError()->getMessage());
                        const auto failure = publishResult.getError().value();
                        recordSpanError(animationSpan, failure.getMessage(), "ExchangeAnimationPublishFailed",
                                        failure.getCode());
                    } else {
                        exchangeAnimationId = animation.id;
                        if (animationSpan) {
                            animationSpan->setAttribute("animation.id", animation.id);
                            animationSpan->setAttribute("animation.frames",
                                                        static_cast<int64_t>(concatenated.totalFrames));
                            animationSpan->setAttribute("animation.tracks",
                                                        static_cast<int64_t>(animation.tracks.size()));
                            animationSpan->setSuccess();
                        }
                    }
                }
            }
        }
    }
    exchange.parts = parts;

    // Best-effort like the insert in start(): the speech already played, so a
    // failed record update must never turn /finish into an error.
    auto finalizeResult = creatures::storage::finalizeAdHocExchange(exchange, finishSpan);
    if (!finalizeResult.isSuccess()) {
        warn("Unable to finalize ad-hoc exchange {}: {}", sessionId_, finalizeResult.getError()->getMessage());
        if (finishSpan) {
            finishSpan->setAttribute("exchange.persistence.outcome", "failed");
            finishSpan->setAttribute("exchange.persistence.error", finalizeResult.getError()->getMessage());
        }
    } else if (finishSpan) {
        finishSpan->setAttribute("exchange.persistence.outcome", "success");
    }

    if (finishSpan) {
        finishSpan->setAttribute("session.id", sessionId_);
        finishSpan->setAttribute("creature.id", participants_.front().creatureId);
        finishSpan->setAttribute("animations.built", static_cast<int64_t>(parts.size()));
        finishSpan->setAttribute("exchange.status", exchange.status);
        finishSpan->setAttribute("exchange.degraded", exchange.status != EXCHANGE_STATUS_READY);
        if (!exchangeAnimationId.empty())
            finishSpan->setAttribute("exchange.animation.id", exchangeAnimationId);
        finishSpan->setSuccess();
    }
    if (span_) {
        span_->setAttribute("exchange.status", exchange.status);
        span_->setAttribute("exchange.parts.rendered", static_cast<int64_t>(parts.size()));
        span_->setAttribute("exchange.parts.total", static_cast<int64_t>(chunksReceived));
        span_->setAttribute("session.degraded", exchange.status != EXCHANGE_STATUS_READY);
    }
    lifecycleCompleted_.store(true);

    info("StreamingAdHocSession finished: session={}, {}/{} sentences rendered, exchange '{}'", sessionId_,
         parts.size(), chunksReceived, exchange.status);

    StreamingFinishResult summary;
    summary.lastAnimationId = lastAnimationId;
    summary.exchangeAnimationId = exchangeAnimationId;
    summary.exchangeStatus = exchange.status;
    summary.partsRendered = static_cast<int>(parts.size());
    summary.partsTotal = chunksReceived;
    return summary;
}

// --- StreamingAdHocSessionManager ---

StreamingAdHocSessionManager &StreamingAdHocSessionManager::instance() {
    static StreamingAdHocSessionManager mgr;
    return mgr;
}

Result<std::shared_ptr<StreamingAdHocSession>>
StreamingAdHocSessionManager::createSession(const std::string &creatureId, bool resumePlaylist,
                                            std::shared_ptr<RequestSpan> parentSpan) {
    StreamingSessionConfig config;
    config.creatureIds = {creatureId};
    config.resumePlaylist = resumePlaylist;
    return createSession(std::move(config), std::move(parentSpan));
}

Result<std::shared_ptr<StreamingAdHocSession>>
StreamingAdHocSessionManager::createSession(StreamingSessionConfig config, std::shared_ptr<RequestSpan> parentSpan) {
    std::vector<std::shared_ptr<StreamingAdHocSession>> expiredSessions;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        if (iterator->second->tryExpire(now)) {
            expiredSessions.push_back(std::move(iterator->second));
            iterator = sessions_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (sessions_.size() >= MAX_ACTIVE_STREAMING_AD_HOC_SESSIONS) {
        return Result<std::shared_ptr<StreamingAdHocSession>>{
            ServerError(ServerError::Conflict, fmt::format("Streaming speech already has {} active sessions",
                                                           MAX_ACTIVE_STREAMING_AD_HOC_SESSIONS))};
    }
    auto sessionId = util::generateUUID();
    auto session = std::make_shared<StreamingAdHocSession>(sessionId, std::move(config), parentSpan);
    sessions_[sessionId] = session;
    return Result<std::shared_ptr<StreamingAdHocSession>>{session};
}

std::shared_ptr<StreamingAdHocSession> StreamingAdHocSessionManager::getSession(const std::string &sessionId) {
    std::shared_ptr<StreamingAdHocSession> expiredSession;
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return nullptr;
    }
    if (!it->second->tryRenewClientLease(std::chrono::steady_clock::now())) {
        expiredSession = std::move(it->second);
        sessions_.erase(it);
        lock.unlock();
        return nullptr;
    }
    return it->second;
}

void StreamingAdHocSessionManager::removeSession(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
}

} // namespace creatures::voice
