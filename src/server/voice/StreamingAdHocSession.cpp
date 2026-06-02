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
#include "model/Animation.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/storage/Storage.h"
#include "server/voice/SpeechTrackBuilder.h"
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
extern std::shared_ptr<util::AudioCache> audioCache;
} // namespace creatures

namespace creatures::voice {

namespace {

std::string slugify(const std::string &value, std::size_t maxLength = 40) {
    std::string slug;
    slug.reserve(std::min<std::size_t>(value.size(), maxLength));
    bool lastDash = false;
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            lastDash = false;
        } else if (std::isspace(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            if (!lastDash && !slug.empty()) {
                slug.push_back('-');
                lastDash = true;
            }
        }
        if (slug.size() >= maxLength)
            break;
    }
    if (slug.empty())
        slug = "speech";
    if (slug.back() == '-')
        slug.pop_back();
    return slug;
}

} // namespace

// --- StreamingAdHocSession ---

StreamingAdHocSession::StreamingAdHocSession(const std::string &sessionId, const std::string &creatureId,
                                             bool resumePlaylist, std::shared_ptr<RequestSpan> parentSpan)
    : sessionId_(sessionId), creatureId_(creatureId), resumePlaylist_(resumePlaylist) {

    if (parentSpan && creatures::observability) {
        span_ = creatures::observability->createChildOperationSpan("StreamingAdHocSession", parentSpan);
        if (span_) {
            span_->setAttribute("session.id", sessionId);
            span_->setAttribute("creature.id", creatureId);
        }
    }

    info("StreamingAdHocSession created: session={}, creature={}", sessionId, creatureId);
}

StreamingAdHocSession::~StreamingAdHocSession() {
    // Ensure playback thread is joined if still running
    if (playbackThread_.joinable()) {
        finished_.store(true);
        playbackCv_.notify_one();
        playbackThread_.join();
    }

    debug("StreamingAdHocSession destroyed: session={}", sessionId_);
    if (span_) {
        span_->setSuccess();
    }
}

Result<void> StreamingAdHocSession::start() {
    auto startSpan = creatures::observability
                         ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.start", span_)
                         : nullptr;

    // Look up creature
    auto creatureJsonResult = creatures::db->getCreatureJson(creatureId_, startSpan);
    if (!creatureJsonResult.isSuccess()) {
        return Result<void>{creatureJsonResult.getError().value()};
    }
    creatureJson_ = creatureJsonResult.getValue().value();

    auto creatureResult = creatures::db->getCreature(creatureId_, startSpan);
    if (!creatureResult.isSuccess()) {
        return Result<void>{creatureResult.getError().value()};
    }
    creature_ = creatureResult.getValue().value();

    if (!creatureJson_.contains("voice") || creatureJson_["voice"].is_null()) {
        return Result<void>{
            ServerError(ServerError::InvalidData, fmt::format("No voice config for creature {}", creatureId_))};
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
        return Result<void>{ServerError(ServerError::InvalidData, fmt::format("Bad voice config: {}", e.what()))};
    }

    // Validate model supports streaming
    static const std::vector<std::string> nonStreamingModels = {"eleven_v3", "eleven_multilingual_v2",
                                                                "eleven_monolingual_v1", "eleven_multilingual_v1"};
    for (const auto &blocked : nonStreamingModels) {
        if (modelId_ == blocked) {
            return Result<void>{ServerError(ServerError::InvalidData,
                                            fmt::format("Model '{}' does not support WebSocket streaming.", modelId_))};
        }
    }

    // Look up universe for playback
    try {
        auto universePtr = creatures::creatureUniverseMap->get(creatureId_);
        universe_ = *universePtr;
    } catch (const std::exception &e) {
        return Result<void>{ServerError(ServerError::InvalidData,
                                        fmt::format("Creature {} is not registered with a universe.", creatureId_))};
    }

    // Resolve speech-loop base frames via the shared helper (issue #15).
    // Returns the decoded body track + the base animation's id + ms-per-frame.
    std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    auto resolveResult = resolveSpeechBaseFrames(creature_, *creatures::db, rng, startSpan);
    if (!resolveResult.isSuccess()) {
        return Result<void>{resolveResult.getError().value()};
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

    info("StreamingAdHocSession started: session={}, voice={}, model={}, base_anim={} ({} frames)", sessionId_,
         voiceId_, modelId_, baseAnimationId, decodedBaseFrames_.size());

    if (startSpan) {
        startSpan->setAttribute("voice.id", voiceId_);
        startSpan->setAttribute("voice.model", modelId_);
        startSpan->setAttribute("base_animation.id", baseAnimationId);
        startSpan->setAttribute("base_animation.frames", static_cast<int64_t>(decodedBaseFrames_.size()));
        startSpan->setSuccess();
    }

    return Result<void>{};
}

Result<void> StreamingAdHocSession::addText(const std::string &text) {
    if (!fullText_.empty()) {
        fullText_ += " ";
    }
    fullText_ += text;
    chunksReceived_++;

    int sentenceIndex = chunksReceived_;

    info("StreamingAdHocSession sentence {}: \"{}\" ({} chars)", sentenceIndex, text, text.size());

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
    auto timestamp = fmt::format("{:%Y%m%d%H%M%S}", std::chrono::system_clock::now());

    auto future =
        std::async(std::launch::async, [this, text, sentenceIndex, creatureName, timestamp]() -> Result<Animation> {
            auto sentenceSpan =
                creatures::observability
                    ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.sentence", span_)
                    : nullptr;
            if (sentenceSpan) {
                sentenceSpan->setAttribute("sentence.index", static_cast<int64_t>(sentenceIndex));
                sentenceSpan->setAttribute("sentence.length", static_cast<int64_t>(text.size()));
            }

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
                if (sentenceSpan)
                    sentenceSpan->setError(ttsResult.getError()->getMessage());
                // Unblock next sentence's waits
                std::lock_guard<std::mutex> lock(offsetMutex_);
                if (sentenceIndex <= static_cast<int>(offsetPromises_.size())) {
                    offsetPromises_[sentenceIndex - 1].set_value(0);
                }
                if (sentenceIndex <= static_cast<int>(requestIdPromises_.size())) {
                    requestIdPromises_[sentenceIndex - 1].set_value("");
                }
                return Result<Animation>{ttsResult.getError().value()};
            }
            const auto tts = ttsResult.getValue().value();

            // 2. Wrap raw PCM into a 17-channel WAV (in-process; previously
            // ffmpeg via AudioConverter::convertMp3ToWav). See issue #12.
            auto tempDir = std::filesystem::temp_directory_path() / "creature-adhoc" / sessionId_;
            std::filesystem::create_directories(tempDir);

            auto wavPath = tempDir / fmt::format("s{}.wav", sentenceIndex);
            auto convertResult = writePcmToMultichannelWav(tts.audioData, wavPath, audioChannel_, 48000);
            if (!convertResult.isSuccess()) {
                if (sentenceSpan)
                    sentenceSpan->setError(convertResult.getError()->getMessage());
                std::lock_guard<std::mutex> lock(offsetMutex_);
                if (sentenceIndex <= static_cast<int>(offsetPromises_.size())) {
                    offsetPromises_[sentenceIndex - 1].set_value(0);
                }
                return Result<Animation>{convertResult.getError().value()};
            }

            // 3. Opus encoding (parallel across channels)
            creatures::rtp::AudioStreamBuffer::loadFromWavFile(wavPath.string(), sentenceSpan);

            // 4. Lip sync from alignment
            std::vector<RhubarbMouthCue> mouthCues;
            if (!tts.charTimings.empty()) {
                mouthCues = textToViseme_.charTimingsToMouthCues(tts.charTimings);
            }
            RhubarbSoundData lipSyncData;
            lipSyncData.metadata.soundFile = wavPath.filename().string();
            lipSyncData.metadata.duration = tts.audioDurationSeconds;
            lipSyncData.mouthCues = mouthCues;

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

            SoundDataProcessor processor;
            auto mouthData = processor.processSoundData(lipSyncData, msPerFrame_, targetFrames);

            // Shared frame-build via the speech track builder (issue #15).
            // mouth_slot bounds check + body cycle + mouth-byte insertion all
            // live in one place now.
            SpeechTrackInput trackInput;
            trackInput.baseFrames = decodedBaseFrames_;
            trackInput.mouthBytes = mouthData;
            trackInput.mouthSlot = creature_.mouth_slot;
            trackInput.totalFrames = targetFrames;
            trackInput.creatureId = creatureId_;
            trackInput.animationId = ""; // stamped onto the Animation below
            SpeechTrackOptions trackOptions;
            trackOptions.startOffset = baseOffset;
            auto trackResult = buildSpeechTrack(trackInput, trackOptions, sentenceSpan);
            if (!trackResult.isSuccess()) {
                return Result<Animation>{trackResult.getError().value()};
            }
            const std::size_t endOffset = trackResult.getValue()->endOffset;
            std::vector<std::string> encodedFrames = std::move(trackResult.getValue()->track.frames);

            // 7. Signal next sentence with our ending offset and request ID
            {
                std::lock_guard<std::mutex> lock(offsetMutex_);
                offsetPromises_[sentenceIndex - 1].set_value(endOffset);
                requestIdPromises_[sentenceIndex - 1].set_value(tts.requestId);
            }

            // 8. Build animation object
            auto textSlug = slugify(tts.alignmentText.empty() ? text : tts.alignmentText);
            Animation animation = baseAnimation_;
            animation.id = util::generateUUID();
            animation.metadata.animation_id = animation.id;
            animation.metadata.title =
                fmt::format("{} - {} - s{} - {}", creatureName, timestamp, sentenceIndex, textSlug);
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

            // 9. Insert into DB. Storage facade pairs the insert + invalidations
            // so each sentence's clients learn about the new artifact ASAP
            // (issue #11).
            creatures::storage::publishAdHocAnimation(animation, sentenceSpan);

            if (sentenceSpan) {
                sentenceSpan->setAttribute("animation.id", animation.id);
                sentenceSpan->setAttribute("animation.frames", static_cast<int64_t>(targetFrames));
                sentenceSpan->setAttribute("base_frame_offset", static_cast<int64_t>(baseOffset));
                sentenceSpan->setSuccess();
            }

            info("Sentence {} animation ready: {} frames, offset {}, {:.2f}s", sentenceIndex, targetFrames, baseOffset,
                 tts.audioDurationSeconds);

            return animation;
        });

    {
        std::lock_guard<std::mutex> lock(futuresMutex_);
        sentenceFutures_.push_back(std::move(future));
    }

    // Spawn the playback thread on the first sentence. It will start waiting
    // for sentence 1's future to resolve and trigger playback immediately.
    if (sentenceIndex == 1) {
        playbackThread_ = std::thread(&StreamingAdHocSession::playbackThreadFunc, this);
    }

    // Wake the playback thread so it knows a new future is available.
    // The future is already in the vector (pushed under lock above), so the
    // playback thread's predicate will see it when it re-checks.
    playbackCv_.notify_one();

    debug("Sentence {} queued for pipelined playback", sentenceIndex);

    return Result<void>{};
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
            lock.unlock();

            int sentenceIndex = static_cast<int>(nextIndex + 1);

            auto animResult = future.get();
            if (!animResult.isSuccess()) {
                warn("Sentence {} failed: {}", sentenceIndex, animResult.getError()->getMessage());
                lock.lock();
                nextIndex++;
                continue;
            }
            auto animation = animResult.getValue().value();
            lastAnimationId = animation.id;

            if (nextIndex == 0) {
                info("Sentence {}: interrupt() for immediate playback (pipelined!)", sentenceIndex);
                auto sessionResult = creatures::sessionManager->interrupt(universe_, animation, resumePlaylist_);
                if (!sessionResult.isSuccess()) {
                    warn("Sentence {} playback failed: {}", sentenceIndex, sessionResult.getError()->getMessage());
                }
            } else {
                info("Sentence {}: queueAnimation() for chained playback", sentenceIndex);
                creatures::sessionManager->queueAnimation(universe_, animation);
            }

            lock.lock();
            nextIndex++;
        }

        // If finish() has been called and we've processed everything, we're done
        if (finished_.load() && nextIndex >= sentenceFutures_.size()) {
            break;
        }
    }

    info("Playback thread finished for session {} (last animation: {})", sessionId_, lastAnimationId);
}

Result<std::string> StreamingAdHocSession::finish() {
    auto finishSpan = creatures::observability
                          ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.finish", span_)
                          : nullptr;

    if (fullText_.empty()) {
        return Result<std::string>{ServerError(ServerError::InvalidData, "No text was added to the session")};
    }

    if (finishSpan) {
        finishSpan->setAttribute("text.length", static_cast<int64_t>(fullText_.size()));
        finishSpan->setAttribute("text.sentences", static_cast<int64_t>(chunksReceived_));
    }

    info("StreamingAdHocSession finishing: session={}, {} sentences, signaling playback thread...", sessionId_,
         chunksReceived_);

    // Write transcript
    auto tempDir = std::filesystem::temp_directory_path() / "creature-adhoc" / sessionId_;
    std::filesystem::create_directories(tempDir);
    {
        std::ofstream f(tempDir / "transcript.txt");
        f << fullText_;
    }

    // Signal the playback thread that no more sentences are coming
    finished_.store(true);
    playbackCv_.notify_one();

    // Wait for the playback thread to finish processing all animations
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }

    // No invalidations fired here — each sentence's publishAdHocAnimation above
    // already invalidates AdHocAnimationList + AdHocSoundList as the chunk lands.

    // Determine the last animation ID
    std::string lastAnimationId;
    {
        std::lock_guard<std::mutex> lock(futuresMutex_);
        // The playback thread already consumed all futures, but we tracked
        // the count. The last animation ID was logged by the playback thread.
        // For the response, we just report success.
    }

    if (finishSpan) {
        finishSpan->setAttribute("animations_built", static_cast<int64_t>(chunksReceived_));
        finishSpan->setSuccess();
    }

    info("StreamingAdHocSession finished: session={}, {} sentences processed", sessionId_, chunksReceived_);

    // Return a non-empty string to indicate success; the actual animation IDs
    // were handled by the playback thread
    return std::string("pipelined-playback");
}

// --- StreamingAdHocSessionManager ---

StreamingAdHocSessionManager &StreamingAdHocSessionManager::instance() {
    static StreamingAdHocSessionManager mgr;
    return mgr;
}

std::shared_ptr<StreamingAdHocSession>
StreamingAdHocSessionManager::createSession(const std::string &creatureId, bool resumePlaylist,
                                            std::shared_ptr<RequestSpan> parentSpan) {
    auto sessionId = util::generateUUID();
    auto session = std::make_shared<StreamingAdHocSession>(sessionId, creatureId, resumePlaylist, parentSpan);

    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[sessionId] = session;
    return session;
}

std::shared_ptr<StreamingAdHocSession> StreamingAdHocSessionManager::getSession(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

void StreamingAdHocSessionManager::removeSession(const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
}

} // namespace creatures::voice
