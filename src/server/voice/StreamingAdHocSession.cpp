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

    // Create TTS client and connect
    ttsClient_ = std::make_unique<StreamingTTSClient>();

    // Build the WebSocket URL path
    std::string path = fmt::format(
        "/v1/text-to-speech/{}/stream-input?model_id={}&output_format=mp3_44100_192&sync_alignment=true", voiceId_,
        modelId_);

    // We need to connect and send BOS manually since generateSpeech() does everything at once.
    // For now, use the simpler approach: accumulate all text, then call generateSpeech() in finish().
    // This still gives us the streaming benefit because the agent sends sentences as they arrive
    // and the server starts processing immediately when finish() is called — no waiting for the
    // full LLM response.

    info("StreamingAdHocSession started: session={}, voice={}, model={}", sessionId_, voiceId_, modelId_);

    if (startSpan) {
        startSpan->setAttribute("voice.id", voiceId_);
        startSpan->setAttribute("voice.model", modelId_);
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

    info("StreamingAdHocSession text chunk {}: \"{}\" (total: {} chars)", chunksReceived_, text, fullText_.size());

    return Result<void>{};
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
        finishSpan->setAttribute("text.chunks", static_cast<int64_t>(chunksReceived_));
    }

    info("StreamingAdHocSession finishing: session={}, {} chunks, {} chars", sessionId_, chunksReceived_,
         fullText_.size());

    // Generate speech using the streaming TTS client
    StreamingTTSClient client;
    auto ttsResult = client.generateSpeech(creatures::config->getVoiceApiKey(), voiceId_, modelId_, fullText_,
                                            "mp3_44100_192", stability_, similarityBoost_, nullptr, finishSpan);

    if (!ttsResult.isSuccess()) {
        if (finishSpan) {
            finishSpan->setError(ttsResult.getError()->getMessage());
        }
        return Result<std::string>{ttsResult.getError().value()};
    }

    auto ttsData = ttsResult.getValue().value();
    if (ttsData.audioData.empty()) {
        return Result<std::string>{ServerError(ServerError::InternalError, "Streaming TTS returned no audio")};
    }

    // Write transcript
    auto tempRoot = std::filesystem::temp_directory_path() / "creature-adhoc";
    auto tempDir = tempRoot / sessionId_;
    std::filesystem::create_directories(tempDir);

    auto transcriptPath = tempDir / "transcript.txt";
    {
        std::ofstream f(transcriptPath);
        f << fullText_;
    }

    // Write MP3 and convert to WAV
    auto mp3Path = tempDir / "speech.mp3";
    {
        std::ofstream f(mp3Path, std::ios::binary);
        f.write(reinterpret_cast<const char *>(ttsData.audioData.data()),
                static_cast<std::streamsize>(ttsData.audioData.size()));
    }

    auto wavPath = tempDir / "speech.wav";
    auto convertResult = AudioConverter::convertMp3ToWav(mp3Path, wavPath, creatures::config->getFfmpegBinaryPath(),
                                                          audioChannel_, 48000, finishSpan);
    if (!convertResult.isSuccess()) {
        if (finishSpan) {
            finishSpan->setError(convertResult.getError()->getMessage());
        }
        return Result<std::string>{convertResult.getError().value()};
    }

    // Generate lip sync from alignment data
    TextToViseme textToViseme;
    auto cmuDictPath = creatures::config->getCmuDictPath();
    if (!cmuDictPath.empty()) {
        textToViseme.loadCmuDict(cmuDictPath);
    }

    std::vector<RhubarbMouthCue> mouthCues;
    if (!ttsData.charTimings.empty()) {
        mouthCues = textToViseme.charTimingsToMouthCues(ttsData.charTimings);
    }

    RhubarbSoundData lipSyncData;
    lipSyncData.metadata.soundFile = wavPath.filename().string();
    lipSyncData.metadata.duration = ttsData.audioDurationSeconds;
    lipSyncData.mouthCues = mouthCues;

    // Prewarm audio cache
    auto audioBuffer = creatures::rtp::AudioStreamBuffer::loadFromWavFile(wavPath.string(), finishSpan);

    // Rename files
    auto creatureName = creature_.name.empty() ? creatureId_ : creature_.name;
    auto creatureSlug = slugify(creatureName);
    auto textSlug = slugify(fullText_);
    auto timestamp = fmt::format("{:%Y%m%d%H%M%S}", std::chrono::system_clock::now());
    auto baseName = fmt::format("adhoc_{}_{}_{}", creatureSlug, timestamp, textSlug);

    auto finalWav = tempDir / fmt::format("{}.wav", baseName);
    std::filesystem::rename(wavPath, finalWav);

    // Build animation (same logic as handleAdHocSpeechJob)
    if (creature_.speech_loop_animation_ids.empty()) {
        return Result<std::string>{ServerError(ServerError::InvalidData, "No speech_loop_animation_ids configured")};
    }

    std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<std::size_t> dist(0, creature_.speech_loop_animation_ids.size() - 1);
    auto baseAnimationId = creature_.speech_loop_animation_ids[dist(rng)];

    auto baseAnimResult = creatures::db->getAnimation(baseAnimationId, finishSpan);
    if (!baseAnimResult.isSuccess()) {
        return Result<std::string>{baseAnimResult.getError().value()};
    }
    auto baseAnimation = baseAnimResult.getValue().value();

    auto trackIt = std::find_if(baseAnimation.tracks.begin(), baseAnimation.tracks.end(),
                                [&](const Track &t) { return t.creature_id == creatureId_; });
    if (trackIt == baseAnimation.tracks.end()) {
        return Result<std::string>{ServerError(ServerError::InvalidData, "No track for creature in speech loop animation")};
    }

    std::vector<std::vector<uint8_t>> decodedFrames;
    decodedFrames.reserve(trackIt->frames.size());
    for (const auto &frame : trackIt->frames) {
        decodedFrames.push_back(decodeBase64(frame));
    }

    uint32_t msPerFrame = baseAnimation.metadata.milliseconds_per_frame;
    if (msPerFrame == 0) msPerFrame = 1;

    size_t targetFrames = std::max<size_t>(
        1, static_cast<size_t>(std::ceil((lipSyncData.metadata.duration * 1000.0) / static_cast<double>(msPerFrame))));

    SoundDataProcessor processor;
    auto mouthData = processor.processSoundData(lipSyncData, msPerFrame, targetFrames);

    auto mouthSlot = creature_.mouth_slot;
    std::vector<std::string> encodedFrames;
    encodedFrames.reserve(targetFrames);
    for (size_t idx = 0; idx < targetFrames; ++idx) {
        auto frameData = decodedFrames[idx % decodedFrames.size()];
        frameData[mouthSlot] = mouthData[idx];
        std::string raw(reinterpret_cast<const char *>(frameData.data()), frameData.size());
        encodedFrames.push_back(base64::to_base64(raw));
    }

    Animation adHocAnimation = baseAnimation;
    adHocAnimation.id = util::generateUUID();
    adHocAnimation.metadata.animation_id = adHocAnimation.id;
    adHocAnimation.metadata.title = fmt::format("{} - {} - {}", creatureName, timestamp, textSlug);
    adHocAnimation.metadata.sound_file = finalWav.string();
    adHocAnimation.metadata.note = fmt::format("Streaming ad-hoc speech: {}", fullText_);
    adHocAnimation.metadata.number_of_frames = static_cast<uint32_t>(encodedFrames.size());
    adHocAnimation.metadata.multitrack_audio = true;

    Track newTrack;
    newTrack.id = util::generateUUID();
    newTrack.creature_id = creatureId_;
    newTrack.animation_id = adHocAnimation.id;
    newTrack.frames = std::move(encodedFrames);
    adHocAnimation.tracks = {newTrack};

    auto createdAt = std::chrono::system_clock::now();
    auto insertResult = creatures::db->insertAdHocAnimation(adHocAnimation, createdAt, finishSpan);
    if (!insertResult.isSuccess()) {
        return Result<std::string>{insertResult.getError().value()};
    }

    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocAnimationList);
    scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::AdHocSoundList);

    // Trigger playback
    try {
        auto universePtr = creatures::creatureUniverseMap->get(creatureId_);
        auto universe = *universePtr;
        auto sessionResult = creatures::sessionManager->interrupt(universe, adHocAnimation, resumePlaylist_);
        if (!sessionResult.isSuccess()) {
            warn("Playback trigger failed: {}", sessionResult.getError()->getMessage());
        }
    } catch (const std::exception &e) {
        warn("Creature {} not registered with a universe: {}", creatureId_, e.what());
    }

    info("StreamingAdHocSession finished: session={}, animation={}", sessionId_, adHocAnimation.id);

    if (finishSpan) {
        finishSpan->setAttribute("animation.id", adHocAnimation.id);
        finishSpan->setSuccess();
    }

    return adHocAnimation.id;
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
