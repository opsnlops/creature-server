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

#include "AudioConverter.h"
#include "RhubarbData.h"
#include "SoundDataProcessor.h"
#include "model/Animation.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
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
        if (slug.size() >= maxLength) break;
    }
    if (slug.empty()) slug = "speech";
    if (slug.back() == '-') slug.pop_back();
    return slug;
}

} // namespace

// --- StreamingAdHocSession ---

StreamingAdHocSession::StreamingAdHocSession(const std::string &sessionId, const std::string &creatureId,
                                              bool resumePlaylist, std::shared_ptr<RequestSpan> parentSpan)
    : sessionId_(sessionId), creatureId_(creatureId), resumePlaylist_(resumePlaylist) {

    if (parentSpan && creatures::observability) {
        span_ = creatures::observability->createChildOperationSpan("StreamingAdHocSession",
                                                                    parentSpan);
        if (span_) {
            span_->setAttribute("session.id", sessionId);
            span_->setAttribute("creature.id", creatureId);
        }
    }

    info("StreamingAdHocSession created: session={}, creature={}", sessionId, creatureId);
}

StreamingAdHocSession::~StreamingAdHocSession() {
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
        return Result<void>{ServerError(ServerError::InvalidData,
                                         fmt::format("No voice config for creature {}", creatureId_))};
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
        return Result<void>{ServerError(ServerError::InvalidData,
                                         fmt::format("Bad voice config: {}", e.what()))};
    }

    // Validate model supports streaming
    static const std::vector<std::string> nonStreamingModels = {"eleven_v3", "eleven_multilingual_v2",
                                                                 "eleven_monolingual_v1", "eleven_multilingual_v1"};
    for (const auto &blocked : nonStreamingModels) {
        if (modelId_ == blocked) {
            return Result<void>{ServerError(
                ServerError::InvalidData,
                fmt::format("Model '{}' does not support WebSocket streaming. Use eleven_turbo_v2_5 or eleven_flash_v2_5.",
                            modelId_))};
        }
    }

    // Look up universe for playback
    try {
        auto universePtr = creatures::creatureUniverseMap->get(creatureId_);
        universe_ = *universePtr;
    } catch (const std::exception &e) {
        return Result<void>{ServerError(
            ServerError::InvalidData,
            fmt::format("Creature {} is not registered with a universe. Is the controller online?", creatureId_))};
    }

    // Load base animation (reused for all sentences)
    if (creature_.speech_loop_animation_ids.empty()) {
        return Result<void>{ServerError(ServerError::InvalidData, "No speech_loop_animation_ids configured")};
    }

    std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<std::size_t> dist(0, creature_.speech_loop_animation_ids.size() - 1);
    auto baseAnimationId = creature_.speech_loop_animation_ids[dist(rng)];

    auto baseAnimResult = creatures::db->getAnimation(baseAnimationId, startSpan);
    if (!baseAnimResult.isSuccess()) {
        return Result<void>{baseAnimResult.getError().value()};
    }
    baseAnimation_ = baseAnimResult.getValue().value();

    auto trackIt = std::find_if(baseAnimation_.tracks.begin(), baseAnimation_.tracks.end(),
                                [&](const Track &t) { return t.creature_id == creatureId_; });
    if (trackIt == baseAnimation_.tracks.end()) {
        return Result<void>{ServerError(ServerError::InvalidData, "No track for creature in speech loop animation")};
    }

    // Decode base frames once
    decodedBaseFrames_.reserve(trackIt->frames.size());
    for (const auto &frame : trackIt->frames) {
        decodedBaseFrames_.push_back(decodeBase64(frame));
    }

    msPerFrame_ = baseAnimation_.metadata.milliseconds_per_frame;
    if (msPerFrame_ == 0) msPerFrame_ = 1;

    // Load CMU dictionary for lip sync
    auto cmuDictPath = creatures::config->getCmuDictPath();
    if (!cmuDictPath.empty()) {
        textToViseme_.loadCmuDict(cmuDictPath);
    }

    info("StreamingAdHocSession started: session={}, voice={}, model={}, base_anim={} ({} frames)",
         sessionId_, voiceId_, modelId_, baseAnimationId, decodedBaseFrames_.size());

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

    // Kick off TTS+animation pipeline in a background thread
    auto future = std::async(std::launch::async,
                              [this, text, sentenceIndex]() { return processSentence(text, sentenceIndex); });

    {
        std::lock_guard<std::mutex> lock(futuresMutex_);
        sentenceFutures_.push_back(std::move(future));
    }

    return Result<void>{};
}

Result<std::string> StreamingAdHocSession::processSentence(const std::string &text, int sentenceIndex) {
    auto sentenceSpan = creatures::observability
                            ? creatures::observability->createChildOperationSpan(
                                  fmt::format("StreamingAdHocSession.sentence.{}", sentenceIndex), span_)
                            : nullptr;
    if (sentenceSpan) {
        sentenceSpan->setAttribute("sentence.index", static_cast<int64_t>(sentenceIndex));
        sentenceSpan->setAttribute("sentence.text", text);
        sentenceSpan->setAttribute("sentence.text_length", static_cast<int64_t>(text.size()));
    }

    // TTS
    StreamingTTSClient client;
    auto ttsResult = client.generateSpeech(creatures::config->getVoiceApiKey(), voiceId_, modelId_, text,
                                            "mp3_44100_192", stability_, similarityBoost_, nullptr, sentenceSpan);
    if (!ttsResult.isSuccess()) {
        if (sentenceSpan) {
            sentenceSpan->setError(ttsResult.getError()->getMessage());
        }
        return Result<std::string>{ttsResult.getError().value()};
    }

    auto ttsData = ttsResult.getValue().value();
    if (ttsData.audioData.empty()) {
        auto msg = fmt::format("Sentence {} TTS returned no audio", sentenceIndex);
        if (sentenceSpan) {
            sentenceSpan->setError(msg);
        }
        return Result<std::string>{ServerError(ServerError::InternalError, msg)};
    }

    // Build animation from TTS result
    auto animResult = buildSentenceAnimation(ttsData, text, sentenceIndex, sentenceSpan);
    if (!animResult.isSuccess()) {
        return Result<std::string>{animResult.getError().value()};
    }
    auto animation = animResult.getValue().value();

    // Insert into DB for archival
    auto createdAt = std::chrono::system_clock::now();
    auto insertResult = creatures::db->insertAdHocAnimation(animation, createdAt, sentenceSpan);
    if (!insertResult.isSuccess()) {
        warn("Failed to archive sentence {} animation: {}", sentenceIndex, insertResult.getError()->getMessage());
        // Non-fatal — continue with playback
    }
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocAnimationList);
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocSoundList);

    // Trigger playback
    bool isFirst = firstSentence_.exchange(false);
    if (isFirst) {
        // First sentence: interrupt current playback
        info("Sentence {}: triggering initial playback on universe {}", sentenceIndex, universe_);
        auto sessionResult = creatures::sessionManager->interrupt(universe_, animation, resumePlaylist_);
        if (!sessionResult.isSuccess()) {
            warn("Sentence {} playback trigger failed: {}", sentenceIndex, sessionResult.getError()->getMessage());
        } else {
            // Set up finish callback to play next pending animation
            auto playbackSession = sessionResult.getValue().value();
            playbackSession->setOnFinishCallback([this]() { playNextPending(); });
        }
    } else {
        // Subsequent sentences: queue for chained playback
        info("Sentence {}: queuing for chained playback", sentenceIndex);
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingAnimations_.push(animation);
        }
        pendingCv_.notify_one();
    }

    lastAnimationId_ = animation.id;

    if (sentenceSpan) {
        sentenceSpan->setAttribute("animation.id", animation.id);
        sentenceSpan->setSuccess();
    }

    return animation.id;
}

Result<Animation> StreamingAdHocSession::buildSentenceAnimation(const StreamingTTSResult &ttsData,
                                                                 const std::string &text, int sentenceIndex,
                                                                 std::shared_ptr<OperationSpan> parentSpan) {
    auto buildSpan = creatures::observability
                         ? creatures::observability->createChildOperationSpan(
                               fmt::format("StreamingAdHocSession.buildAnimation.{}", sentenceIndex), parentSpan)
                         : nullptr;

    // Write MP3 and convert to WAV
    auto tempRoot = std::filesystem::temp_directory_path() / "creature-adhoc";
    auto tempDir = tempRoot / sessionId_;
    std::filesystem::create_directories(tempDir);

    auto mp3Path = tempDir / fmt::format("sentence_{}.mp3", sentenceIndex);
    {
        std::ofstream f(mp3Path, std::ios::binary);
        f.write(reinterpret_cast<const char *>(ttsData.audioData.data()),
                static_cast<std::streamsize>(ttsData.audioData.size()));
    }

    auto wavPath = tempDir / fmt::format("sentence_{}.wav", sentenceIndex);
    auto convertResult = AudioConverter::convertMp3ToWav(mp3Path, wavPath, creatures::config->getFfmpegBinaryPath(),
                                                          audioChannel_, 48000, buildSpan);
    if (!convertResult.isSuccess()) {
        if (buildSpan) {
            buildSpan->setError(convertResult.getError()->getMessage());
        }
        return Result<Animation>{convertResult.getError().value()};
    }

    // Prewarm audio cache (Opus encoding)
    auto audioBuffer = creatures::rtp::AudioStreamBuffer::loadFromWavFile(wavPath.string(), buildSpan);

    // Generate lip sync from alignment data
    std::vector<RhubarbMouthCue> mouthCues;
    if (!ttsData.charTimings.empty()) {
        mouthCues = textToViseme_.charTimingsToMouthCues(ttsData.charTimings);
    }

    RhubarbSoundData lipSyncData;
    lipSyncData.metadata.soundFile = wavPath.filename().string();
    lipSyncData.metadata.duration = ttsData.audioDurationSeconds;
    lipSyncData.mouthCues = mouthCues;

    // Calculate target frames for this sentence
    size_t targetFrames = std::max<size_t>(
        1, static_cast<size_t>(std::ceil((lipSyncData.metadata.duration * 1000.0) / static_cast<double>(msPerFrame_))));

    SoundDataProcessor processor;
    auto mouthData = processor.processSoundData(lipSyncData, msPerFrame_, targetFrames);

    // Get current frame offset for seamless body motion
    size_t offset = baseFrameOffset_.load();
    auto mouthSlot = creature_.mouth_slot;

    // Build frames starting from the offset into the base animation loop
    std::vector<std::string> encodedFrames;
    encodedFrames.reserve(targetFrames);
    for (size_t idx = 0; idx < targetFrames; ++idx) {
        auto frameData = decodedBaseFrames_[(offset + idx) % decodedBaseFrames_.size()];
        frameData[mouthSlot] = mouthData[idx];
        std::string raw(reinterpret_cast<const char *>(frameData.data()), frameData.size());
        encodedFrames.push_back(base64::to_base64(raw));
    }

    // Advance the offset for the next sentence
    baseFrameOffset_.store((offset + targetFrames) % decodedBaseFrames_.size());

    // Build animation object
    auto creatureName = creature_.name.empty() ? creatureId_ : creature_.name;
    auto textSlug = slugify(text);
    auto timestamp = fmt::format("{:%Y%m%d%H%M%S}", std::chrono::system_clock::now());

    Animation animation = baseAnimation_;
    animation.id = util::generateUUID();
    animation.metadata.animation_id = animation.id;
    animation.metadata.title = fmt::format("{} - {} - s{} - {}", creatureName, timestamp, sentenceIndex, textSlug);
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

    if (buildSpan) {
        buildSpan->setAttribute("animation.id", animation.id);
        buildSpan->setAttribute("animation.frames", static_cast<int64_t>(targetFrames));
        buildSpan->setAttribute("base_frame_offset", static_cast<int64_t>(offset));
        buildSpan->setAttribute("audio.duration_s", ttsData.audioDurationSeconds);
        buildSpan->setSuccess();
    }

    info("Sentence {} animation built: {} frames, offset {}, {:.2f}s audio", sentenceIndex, targetFrames, offset,
         ttsData.audioDurationSeconds);

    return animation;
}

void StreamingAdHocSession::playNextPending() {
    Animation nextAnimation;

    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pendingAnimations_.empty()) {
            debug("No more pending animations for session {}", sessionId_);
            return;
        }
        nextAnimation = pendingAnimations_.front();
        pendingAnimations_.pop();
    }

    info("Playing next chained animation: {} for session {}", nextAnimation.metadata.title, sessionId_);

    auto sessionResult = creatures::sessionManager->interrupt(universe_, nextAnimation, resumePlaylist_);
    if (!sessionResult.isSuccess()) {
        warn("Chained playback failed: {}", sessionResult.getError()->getMessage());
        return;
    }

    auto playbackSession = sessionResult.getValue().value();
    playbackSession->setOnFinishCallback([this]() { playNextPending(); });
}

Result<std::string> StreamingAdHocSession::finish() {
    auto finishSpan = creatures::observability
                          ? creatures::observability->createChildOperationSpan("StreamingAdHocSession.finish", span_)
                          : nullptr;

    if (fullText_.empty()) {
        return Result<std::string>{ServerError(ServerError::InvalidData, "No text was added to the session")};
    }

    info("StreamingAdHocSession finishing: session={}, {} sentences", sessionId_, chunksReceived_);

    // Wait for all sentence processing to complete
    {
        std::lock_guard<std::mutex> lock(futuresMutex_);
        for (auto &future : sentenceFutures_) {
            if (future.valid()) {
                auto result = future.get();
                if (result.isSuccess()) {
                    lastAnimationId_ = result.getValue().value();
                } else {
                    warn("Sentence processing failed: {}", result.getError()->getMessage());
                }
            }
        }
        sentenceFutures_.clear();
    }

    // Write full transcript
    auto tempRoot = std::filesystem::temp_directory_path() / "creature-adhoc";
    auto tempDir = tempRoot / sessionId_;
    auto transcriptPath = tempDir / "transcript.txt";
    {
        std::ofstream f(transcriptPath);
        f << fullText_;
    }

    if (finishSpan) {
        finishSpan->setAttribute("text.length", static_cast<int64_t>(fullText_.size()));
        finishSpan->setAttribute("text.sentences", static_cast<int64_t>(chunksReceived_));
        finishSpan->setAttribute("last_animation.id", lastAnimationId_);
        finishSpan->setSuccess();
    }

    info("StreamingAdHocSession finished: session={}, last_animation={}", sessionId_, lastAnimationId_);

    return lastAnimationId_;
}

// --- StreamingAdHocSessionManager ---

StreamingAdHocSessionManager &StreamingAdHocSessionManager::instance() {
    static StreamingAdHocSessionManager mgr;
    return mgr;
}

std::shared_ptr<StreamingAdHocSession> StreamingAdHocSessionManager::createSession(
    const std::string &creatureId, bool resumePlaylist, std::shared_ptr<RequestSpan> parentSpan) {
    auto sessionId = util::generateUUID();
    auto session =
        std::make_shared<StreamingAdHocSession>(sessionId, creatureId, resumePlaylist, parentSpan);

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
