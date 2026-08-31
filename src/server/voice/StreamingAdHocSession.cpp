#include "StreamingAdHocSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

#include <base64.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "PcmWavWriter.h"
#include "RhubarbData.h"
#include "SoundDataProcessor.h"
#include "model/AdHocExchange.h"
#include "model/Animation.h"
#include "server/animation/CooperativeAnimationScheduler.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/storage/Storage.h"
#include "server/voice/SpeechTrackBuilder.h"
#include "util/Slugify.h"
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

StreamingAdHocSession::StreamingAdHocSession(const std::string &sessionId, const std::string &creatureId,
                                             bool resumePlaylist, std::shared_ptr<RequestSpan> parentSpan)
    : sessionId_(sessionId), creatureId_(creatureId), resumePlaylist_(resumePlaylist) {

    createdAtNs_ = monotonicNowNs();
    lastClientActivityNs_ = createdAtNs_;

    if (parentSpan && creatures::observability) {
        span_ = creatures::observability->createLinkedOperationSpan("StreamingAdHocSession", parentSpan);
        if (span_) {
            span_->setAttribute("session.id", sessionId);
            span_->setAttribute("creature.id", creatureId);
        }
    }

    info("StreamingAdHocSession created: session={}, creature={}", sessionId, creatureId);
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

void StreamingAdHocSession::resolveFailedSentence(int sentenceIndex) noexcept {
    std::lock_guard<std::mutex> lock(offsetMutex_);
    try {
        offsetPromises_.at(static_cast<std::size_t>(sentenceIndex - 1)).set_value(0);
    } catch (const std::exception &) {
    }
    try {
        requestIdPromises_.at(static_cast<std::size_t>(sentenceIndex - 1)).set_value("");
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

    // Look up creature
    auto creatureJsonResult = creatures::db->getCreatureJson(creatureId_, startSpan);
    if (!creatureJsonResult.isSuccess()) {
        return fail(creatureJsonResult.getError().value(), "CreatureLookupFailed");
    }
    creatureJson_ = creatureJsonResult.getValue().value();

    auto creatureResult = creatures::db->getCreature(creatureId_, startSpan);
    if (!creatureResult.isSuccess()) {
        return fail(creatureResult.getError().value(), "CreatureLookupFailed");
    }
    creature_ = creatureResult.getValue().value();

    if (!creatureJson_.contains("voice") || creatureJson_["voice"].is_null()) {
        return fail(ServerError(ServerError::InvalidData, fmt::format("No voice config for creature {}", creatureId_)),
                    "MissingVoiceConfig");
    }

    // Extract voice config
    try {
        audioChannel_ = creatureJson_.value("audio_channel", static_cast<uint16_t>(1));
        auto voiceConfig = creatureJson_["voice"];
        voiceId_ = voiceConfig["voice_id"].get<std::string>();
        modelId_ = voiceConfig["model_id"].get<std::string>();
        stability_ = voiceConfig["stability"].get<float>();
        similarityBoost_ = voiceConfig["similarity_boost"].get<float>();
    } catch (const std::exception &e) {
        if (startSpan)
            startSpan->recordException(e);
        if (span_)
            span_->recordException(e);
        return fail(ServerError(ServerError::InvalidData, fmt::format("Bad voice config: {}", e.what())),
                    "InvalidVoiceConfig");
    }

    // Validate model supports streaming
    static const std::vector<std::string> nonStreamingModels = {"eleven_v3", "eleven_multilingual_v2",
                                                                "eleven_monolingual_v1", "eleven_multilingual_v1"};
    for (const auto &blocked : nonStreamingModels) {
        if (modelId_ == blocked) {
            return fail(ServerError(ServerError::InvalidData,
                                    fmt::format("Model '{}' does not support WebSocket streaming.", modelId_)),
                        "UnsupportedVoiceModel");
        }
    }

    // Look up universe for playback
    try {
        auto universePtr = creatures::creatureUniverseMap->get(creatureId_);
        universe_ = *universePtr;
    } catch (const std::exception &e) {
        if (startSpan)
            startSpan->recordException(e);
        if (span_)
            span_->recordException(e);
        return fail(ServerError(ServerError::InvalidData,
                                fmt::format("Creature {} is not registered with a universe.", creatureId_)),
                    "CreatureUniverseMissing");
    }

    // Resolve speech-loop base frames via the shared helper (issue #15).
    // Returns the decoded body track + the base animation's id + ms-per-frame.
    std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    auto resolveResult = resolveSpeechBaseFrames(creature_, *creatures::db, rng, startSpan);
    if (!resolveResult.isSuccess()) {
        return fail(resolveResult.getError().value(), "SpeechBaseResolutionFailed");
    }
    auto resolved = resolveResult.getValue().value();
    decodedBaseFrames_ = std::move(resolved.baseFrames);
    baseAnimation_ = std::move(resolved.baseAnimation);
    const std::string baseAnimationId = resolved.baseAnimationId;
    msPerFrame_ = resolved.baseMsPerFrame == 0 ? 1u : resolved.baseMsPerFrame;

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
        exchange.creature_id = creatureId_;
        exchange.creature_name = creature_.name.empty() ? creatureId_ : creature_.name;
        exchange.status = EXCHANGE_STATUS_STREAMING;
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

    info("StreamingAdHocSession started: session={}, voice={}, model={}, base_anim={} ({} frames)", sessionId_,
         voiceId_, modelId_, baseAnimationId, decodedBaseFrames_.size());

    if (startSpan) {
        startSpan->setAttribute("voice.id", voiceId_);
        startSpan->setAttribute("voice.model", modelId_);
        startSpan->setAttribute("animation.base.id", baseAnimationId);
        startSpan->setAttribute("animation.base.frames", static_cast<int64_t>(decodedBaseFrames_.size()));
        startSpan->setSuccess();
    }

    return Result<void>{};
}

Result<void> StreamingAdHocSession::addText(const std::string &text, std::shared_ptr<RequestSpan> triggerSpan) {
    std::lock_guard<std::mutex> stateLock(stateMutex_);
    if (finished_.load()) {
        return Result<void>{ServerError(ServerError::Conflict, "Streaming session is already finishing")};
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
    sentenceTexts_.push_back(text);

    info("StreamingAdHocSession received sentence {} ({} bytes)", sentenceIndex, text.size());

    // Create promise/future pairs for synchronization between sentences:
    // - Frame offset: sentence N waits for N-1's offset before building
    // - Request ID: sentence N uses N-1's ElevenLabs request ID for prosody continuity
    {
        std::lock_guard<std::mutex> lock(offsetMutex_);
        offsetPromises_.emplace_back();
        offsetFutures_.push_back(offsetPromises_.back().get_future().share());
        requestIdPromises_.emplace_back();
        requestIdFutures_.push_back(requestIdPromises_.back().get_future().share());
    }

    // Kick off full pipeline (TTS + WAV wrap + Opus + animation build) in background.
    auto creatureName = creature_.name.empty() ? creatureId_ : creature_.name;

    auto sentenceSpan = creatures::observability ? creatures::observability->createLinkedOperationSpan(
                                                       "StreamingAdHocSession.sentence", std::move(triggerSpan))
                                                 : nullptr;
    if (sentenceSpan) {
        sentenceSpan->setAttribute("session.id", sessionId_);
        sentenceSpan->setAttribute("creature.id", creatureId_);
        sentenceSpan->setAttribute("sentence.index", static_cast<int64_t>(sentenceIndex));
        sentenceSpan->setAttribute("sentence.length", static_cast<int64_t>(text.size()));
    }

    std::packaged_task<Result<Animation>()> renderTask([this, text, sentenceIndex, creatureName, sentenceSpan,
                                                        renderReservation =
                                                            std::move(renderReservation)]() -> Result<Animation> {
        (void)renderReservation;
        try {
            // 1. TTS via REST with previous_request_ids for prosody continuity.
            // Read the previous sentence's request-id future under the lock —
            // concurrent addText() can be doing push_back() which would invalidate
            // an iterator-style access; copying the shared_future locally is safe
            // because shared_future is itself reference-counted.
            std::vector<std::string> prevIds;
            if (sentenceIndex > 1) {
                std::shared_future<std::string> prevRequestIdFuture;
                {
                    std::lock_guard<std::mutex> lock(offsetMutex_);
                    prevRequestIdFuture = requestIdFutures_[sentenceIndex - 2];
                }
                auto prevId = prevRequestIdFuture.get();
                if (!prevId.empty()) {
                    prevIds.push_back(prevId);
                }
            }

            StreamingTTSClient client;
            // Request raw mono 48 kHz S16 PCM directly (issue #12). The
            // 17-channel WAV is wrapped in-process below; no ffmpeg decode hop.
            auto ttsResult =
                client.generateSpeechREST(creatures::config->getVoiceApiKey(), voiceId_, modelId_, text, "pcm_48000",
                                          stability_, similarityBoost_, prevIds, nullptr, sentenceSpan);
            if (!ttsResult.isSuccess()) {
                const auto failure = ttsResult.getError().value();
                lifecycleFailed_.store(true);
                recordSpanError(sentenceSpan, failure.getMessage(), "TextToSpeechFailed", failure.getCode());
                resolveFailedSentence(sentenceIndex);
                return Result<Animation>{ttsResult.getError().value()};
            }
            const auto tts = ttsResult.getValue().value();

            // 2. Wrap raw PCM into a 17-channel WAV (in-process; previously
            // ffmpeg via AudioConverter::convertMp3ToWav). See issue #12.
            auto tempDir = std::filesystem::temp_directory_path() / "creature-adhoc" / sessionId_;
            std::filesystem::create_directories(tempDir);

            auto wavPath = tempDir / fmt::format("s{}.wav", sentenceIndex);
            auto pcmSpan =
                creatures::observability
                    ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.wrapPcm", sentenceSpan)
                    : nullptr;
            if (pcmSpan) {
                pcmSpan->setAttribute("audio.input.bytes", static_cast<int64_t>(tts.audioData.size()));
                pcmSpan->setAttribute("audio.channel", static_cast<int64_t>(audioChannel_));
                pcmSpan->setAttribute("audio.sample.rate", static_cast<int64_t>(48000));
            }
            auto convertResult = writePcmToMultichannelWav(tts.audioData, wavPath, audioChannel_, 48000);
            if (!convertResult.isSuccess()) {
                const auto failure = convertResult.getError().value();
                lifecycleFailed_.store(true);
                recordSpanError(pcmSpan, failure.getMessage(), "PcmWavWriteFailed", failure.getCode());
                recordSpanError(sentenceSpan, failure.getMessage(), "PcmWavWriteFailed", failure.getCode());
                resolveFailedSentence(sentenceIndex);
                return Result<Animation>{convertResult.getError().value()};
            }
            if (pcmSpan)
                pcmSpan->setSuccess();

            // 3. Opus encoding (parallel across channels)
            // Prewarm only — the buffer is discarded here and this sentence's temp
            // path is never loaded again, so it must not consume the retention
            // budget that keeps show audio warm (issue #93).
            creatures::rtp::AudioStreamBuffer::loadFromWavFile(
                wavPath.string(), sentenceSpan, creatures::rtp::AudioStreamBuffer::RetentionIntent::OneShot);

            // 5. Wait for previous sentence's frame offset. Same locking
            // pattern as the request-id read above: copy the future under
            // the mutex, then block on it.
            size_t baseOffset = 0;
            if (sentenceIndex > 1) {
                std::shared_future<size_t> prevOffsetFuture;
                {
                    std::lock_guard<std::mutex> lock(offsetMutex_);
                    prevOffsetFuture = offsetFutures_[sentenceIndex - 2];
                }
                baseOffset = prevOffsetFuture.get();
            }

            // 6. Build animation frames
            size_t targetFrames = std::max<size_t>(
                1,
                static_cast<size_t>(std::ceil((tts.audioDurationSeconds * 1000.0) / static_cast<double>(msPerFrame_))));

            // 4/6. Convert the provider's character alignment into mouth
            // frames. Keep this as one coarse span: per-cue/per-frame spans
            // would add volume without making the pipeline easier to query.
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

            // Shared frame-build via the speech track builder (issue #15).
            // mouth_slot bounds check + body cycle + mouth-byte insertion all
            // live in one place now.
            SpeechTrackInput trackInput;
            trackInput.baseFrames = decodedBaseFrames_;
            trackInput.mouthBytes = mouthData;
            trackInput.mouthSlot = creatures::resolvedMouthSlot(creature_);
            trackInput.totalFrames = targetFrames;
            trackInput.creatureId = creatureId_;
            trackInput.animationId = ""; // stamped onto the Animation below
            SpeechTrackOptions trackOptions;
            trackOptions.startOffset = baseOffset;
            auto trackSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                            "StreamingAdHocSession.buildTrack", sentenceSpan)
                                                      : nullptr;
            if (trackSpan) {
                trackSpan->setAttribute("animation.target.frames", static_cast<int64_t>(targetFrames));
                trackSpan->setAttribute("frame.base.offset", static_cast<int64_t>(baseOffset));
                trackSpan->setAttribute("mouth.slot", static_cast<int64_t>(trackInput.mouthSlot));
            }
            auto trackResult = buildSpeechTrack(trackInput, trackOptions, trackSpan);
            if (!trackResult.isSuccess()) {
                const auto failure = trackResult.getError().value();
                lifecycleFailed_.store(true);
                recordSpanError(trackSpan, failure.getMessage(), "SpeechTrackBuildFailed", failure.getCode());
                recordSpanError(sentenceSpan, failure.getMessage(), "SpeechTrackBuildFailed", failure.getCode());
                resolveFailedSentence(sentenceIndex);
                return Result<Animation>{trackResult.getError().value()};
            }
            const std::size_t endOffset = trackResult.getValue()->endOffset;
            std::vector<std::string> encodedFrames = std::move(trackResult.getValue()->track.frames);
            if (trackSpan) {
                trackSpan->setAttribute("animation.frames", static_cast<int64_t>(encodedFrames.size()));
                trackSpan->setAttribute("frame.end.offset", static_cast<int64_t>(endOffset));
                trackSpan->setSuccess();
            }

            // 7. Signal next sentence with our ending offset and request ID
            {
                std::lock_guard<std::mutex> lock(offsetMutex_);
                offsetPromises_[sentenceIndex - 1].set_value(endOffset);
                requestIdPromises_[sentenceIndex - 1].set_value(tts.requestId);
            }

            // 8. Build animation object
            auto textSlug = util::slugify(tts.alignmentText.empty() ? text : tts.alignmentText, 40, "speech");
            Animation animation = baseAnimation_;
            animation.id = util::generateUUID();
            animation.metadata.animation_id = animation.id;
            // Named after what it is (#126), matching the exchange title shape;
            // the ad-hoc list already shows created_at.
            animation.metadata.title = fmt::format("{} - s{} - {}", creatureName, sentenceIndex, textSlug);
            animation.metadata.sound_file = wavPath.string();
            animation.metadata.note = fmt::format("Streaming sentence {}: {}", sentenceIndex, text);
            animation.metadata.number_of_frames = static_cast<uint32_t>(encodedFrames.size());
            animation.metadata.multitrack_audio = true;

            Track newTrack;
            newTrack.id = util::generateUUID();
            newTrack.creature_id = creatureId_;
            newTrack.animation_id = animation.id;
            newTrack.frames = std::move(encodedFrames);
            animation.tracks = {newTrack};

            // Expiration can race the one already-running render after queued
            // work is discarded. Do not persist or dispatch that stale result.
            if (cancelled_.load(std::memory_order_acquire)) {
                lifecycleFailed_.store(true);
                recordSpanError(sentenceSpan, "Streaming render cancelled after session expiry", "RenderCancelled",
                                ServerError::Conflict);
                return Result<Animation>{ServerError(ServerError::Conflict, "Streaming render cancelled")};
            }

            // 9. Insert into DB. Storage facade pairs the insert + invalidations
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
                sentenceSpan->setAttribute("frame.base.offset", static_cast<int64_t>(baseOffset));
                sentenceSpan->setSuccess();
            }

            info("Sentence {} animation ready: {} frames, offset {}, {:.2f}s", sentenceIndex, targetFrames, baseOffset,
                 tts.audioDurationSeconds);

            return animation;
        } catch (const std::exception &exception) {
            lifecycleFailed_.store(true);
            if (sentenceSpan)
                sentenceSpan->recordException(exception);
            recordSpanError(sentenceSpan, exception.what(), "BackgroundPipelineException", ServerError::InternalError);
            resolveFailedSentence(sentenceIndex);
            return Result<Animation>{ServerError(ServerError::InternalError, "Background speech pipeline failed")};
        } catch (...) {
            lifecycleFailed_.store(true);
            recordSpanError(sentenceSpan, "Unknown background speech pipeline failure",
                            "UnknownBackgroundPipelineException", ServerError::InternalError);
            resolveFailedSentence(sentenceIndex);
            return Result<Animation>{ServerError(ServerError::InternalError, "Background speech pipeline failed")};
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
        std::packaged_task<Result<Animation>()> task;
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

            std::optional<Result<Animation>> animationResult;
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
                resolveFailedSentence(sentenceIndex);
            } catch (...) {
                lifecycleFailed_.store(true);
                error("Sentence {} background pipeline threw an unknown exception", sentenceIndex);
                recordSpanError(sentenceSpan, "Unknown background pipeline exception",
                                cancelled_.load(std::memory_order_acquire) ? "RenderCancelled"
                                                                           : "UnknownBackgroundPipelineException",
                                cancelled_.load(std::memory_order_acquire) ? ServerError::Conflict
                                                                           : ServerError::InternalError);
                resolveFailedSentence(sentenceIndex);
            }
            if (!animationResult || !animationResult->isSuccess()) {
                if (animationResult) {
                    warn("Sentence {} failed: {}", sentenceIndex, animationResult->getError()->getMessage());
                }
                lock.lock();
                sentenceOutcomes_.push_back({false, ""});
                outstandingSentenceWork_.fetch_sub(1, std::memory_order_release);
                nextIndex++;
                continue;
            }
            auto animation = animationResult->getValue().value();
            if (cancelled_.load(std::memory_order_acquire)) {
                lifecycleFailed_.store(true);
                recordSpanError(sentenceSpan, "Playback suppressed after streaming session expiry", "RenderCancelled",
                                ServerError::Conflict);
                lock.lock();
                sentenceOutcomes_.push_back({false, ""});
                outstandingSentenceWork_.fetch_sub(1, std::memory_order_release);
                nextIndex++;
                continue;
            }
            lastAnimationId = animation.id;

            auto playbackSpan =
                creatures::observability
                    ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.playback", sentenceSpan)
                    : nullptr;
            if (playbackSpan) {
                playbackSpan->setAttribute("session.id", sessionId_);
                playbackSpan->setAttribute("creature.id", creatureId_);
                playbackSpan->setAttribute("sentence.index", static_cast<int64_t>(sentenceIndex));
                playbackSpan->setAttribute("animation.id", animation.id);
                playbackSpan->setAttribute("playback.universe", static_cast<int64_t>(universe_));
            }

            bool dispatched = true;
            if (nextIndex == 0) {
                if (playbackSpan)
                    playbackSpan->setAttribute("playback.dispatch.mode", "interrupt");
                info("Sentence {}: interrupt() for immediate playback (pipelined!)", sentenceIndex);
                // Our session id is the chain id: every sentence's playback session
                // carries it, so queue entries and failure cleanup stay scoped to
                // this chain (issue #100).
                auto sessionResult =
                    creatures::sessionManager->interrupt(universe_, animation, resumePlaylist_, nullptr, sessionId_);
                if (!sessionResult.isSuccess()) {
                    warn("Sentence {} playback failed: {}", sentenceIndex, sessionResult.getError()->getMessage());
                    const auto failure = sessionResult.getError().value();
                    recordSpanError(playbackSpan, failure.getMessage(), "PlaybackInterruptFailed", failure.getCode());
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

            if (playbackSpan) {
                playbackSpan->setAttribute("playback.dispatched", dispatched);
                if (dispatched)
                    playbackSpan->setSuccess();
            }
            if (!dispatched)
                lifecycleFailed_.store(true);

            lock.lock();
            sentenceOutcomes_.push_back({dispatched, dispatched ? animation.id : ""});
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

    // Write transcript
    auto tempDir = std::filesystem::temp_directory_path() / "creature-adhoc" / sessionId_;
    std::filesystem::create_directories(tempDir);
    {
        std::ofstream f(tempDir / "transcript.txt");
        f << fullText_;
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

    // No invalidations fired here — each sentence's publishAdHocAnimation above
    // already invalidates AdHocAnimationList + AdHocSoundList as the chunk lands.

    // The playback thread is joined, so every sentence's WAV that will ever
    // exist is on disk now — harvest the outcomes and stitch the exchange
    // (issue #150). No lock needed: nothing else touches these vectors anymore.
    const auto creatureName = creature_.name.empty() ? creatureId_ : creature_.name;
    std::vector<AdHocExchangePart> parts;
    std::vector<std::filesystem::path> partWavs;
    std::string lastAnimationId;
    for (size_t i = 0; i < sentenceOutcomes_.size(); i++) {
        const auto &outcome = sentenceOutcomes_[i];
        if (!outcome.success) {
            continue;
        }
        AdHocExchangePart part;
        part.index = static_cast<uint32_t>(i + 1);
        part.animation_id = outcome.animationId;
        part.text = i < sentenceTexts_.size() ? sentenceTexts_[i] : "";
        parts.push_back(std::move(part));
        partWavs.push_back(tempDir / fmt::format("s{}.wav", i + 1));
        lastAnimationId = outcome.animationId;
    }

    creatures::AdHocExchange exchange;
    exchange.session_id = sessionId_;
    exchange.creature_id = creatureId_;
    exchange.creature_name = creatureName;
    exchange.transcript = fullText_;
    exchange.finished_at_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    // Name the exchange after what it IS (#126), like dialog renders: creature
    // plus the words — with their natural punctuation, because this title
    // lands in the ID3 tags (#157). The download filename slugifies it, so
    // the dashes still show up exactly where they belong. No timestamp —
    // created_at rides in the record, and the filename gets a session-id
    // tail for uniqueness.
    exchange.title = fmt::format("{} - {}", creatureName, util::titleExcerpt(fullText_, 60, "exchange"));

    if (parts.empty()) {
        exchange.status = EXCHANGE_STATUS_FAILED;
    } else {
        // The stitched WAV is the first ad-hoc artifact with real provenance:
        // the #148 tag mapping turns this into TITLE/ARTIST/LYRICS on the MP3.
        WavProvenance provenance;
        provenance.fileUid = sessionId_;
        provenance.take = "exchange";
        provenance.title = exchange.title;
        provenance.tracks = {{audioChannel_, creatureName}};
        for (const auto &part : parts) {
            provenance.script.push_back({creatureName, part.text});
        }
        {
            // The ElevenLabs request ids are already tracked for prosody
            // chaining; all promises are resolved by now (failed sentences
            // resolve to ""), but guard with wait_for anyway — a promise
            // missed on an error path must not hang finish() forever.
            std::lock_guard<std::mutex> lock(offsetMutex_);
            for (const auto &requestIdFuture : requestIdFutures_) {
                if (requestIdFuture.valid() &&
                    requestIdFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    const auto &requestId = requestIdFuture.get();
                    if (!requestId.empty()) {
                        provenance.generationIds.push_back(requestId);
                    }
                }
            }
        }

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
        finishSpan->setAttribute("creature.id", creatureId_);
        finishSpan->setAttribute("animations.built", static_cast<int64_t>(parts.size()));
        finishSpan->setAttribute("exchange.status", exchange.status);
        finishSpan->setAttribute("exchange.degraded", exchange.status != EXCHANGE_STATUS_READY);
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
    auto session = std::make_shared<StreamingAdHocSession>(sessionId, creatureId, resumePlaylist, parentSpan);
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
