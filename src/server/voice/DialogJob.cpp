#include "DialogJob.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "DialogAnimation.h"
#include "DialogClient.h"
#include "DialogPipeline.h"
#include "DialogWav.h"
#include "RhubarbData.h"
#include "SoundDataProcessor.h"
#include "TextToViseme.h"
#include "model/Animation.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "util/cache.h"
#include "util/helpers.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
} // namespace creatures

namespace creatures::voice {

namespace {

/// Maximum unique voice IDs per ElevenLabs Text-to-Dialogue submission (per
/// docs). We enforce on the SCENE (which may be split across chunks) — the
/// limit applies to every individual API call but it's simpler + more useful
/// to author scenes that fit each chunk into the same voice budget.
constexpr std::size_t kMaxUniqueVoicesPerScene = 10;

constexpr uint32_t kDialogSampleRate = 48000;

/// Where the worker writes the assembled 17-channel WAV. The animations table
/// stores this path in metadata.sound_file; playback reads it from there.
const std::filesystem::path kDialogWavDir{"/tmp/creature-dialog"};

/// Wrap raw mono S16LE PCM in a canonical 44-byte PCM WAV header, returning
/// the full file bytes. Used to convert the dialog audio (raw PCM from
/// generateDialog) into the WAV upload that forcedAlignment expects.
std::vector<uint8_t> wrapMonoPcmAsWav(const std::vector<uint8_t> &pcm, uint32_t sampleRate) {
    std::vector<uint8_t> out;
    out.reserve(44 + pcm.size());
    auto u16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto u32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    auto str = [&](const char *s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(static_cast<uint8_t>(s[i]));
        }
    };
    const uint32_t dataLen = static_cast<uint32_t>(pcm.size());
    str("RIFF", 4);
    u32(36 + dataLen);
    str("WAVE", 4);
    str("fmt ", 4);
    u32(16);             // fmt chunk size
    u16(1);              // PCM
    u16(1);              // mono
    u32(sampleRate);     // sample rate
    u32(sampleRate * 2); // byte rate (mono, 16-bit)
    u16(2);              // block align (mono * 2 bytes)
    u16(16);             // bits per sample
    str("data", 4);
    u32(dataLen);
    out.insert(out.end(), pcm.begin(), pcm.end());
    return out;
}

/// Pull the runtime-cached universe for `creatureId` out of the global map.
/// The map is populated when the controller registers — if a creature isn't
/// registered we can't autoplay on it.
std::optional<universe_t> tryGetUniverse(const std::string &creatureId) {
    try {
        auto u = creatures::creatureUniverseMap->get(creatureId);
        if (!u) {
            return std::nullopt;
        }
        return *u;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

} // namespace

DialogJobManager &DialogJobManager::instance() {
    static DialogJobManager singleton;
    return singleton;
}

std::shared_ptr<TextToViseme> DialogJobManager::getTextToViseme() {
    std::lock_guard<std::mutex> lock(visemeMutex_);
    if (textToViseme_ && textToViseme_->isLoaded()) {
        return textToViseme_;
    }
    auto v = std::make_shared<TextToViseme>();
    const auto path = creatures::config->getCmuDictPath();
    if (path.empty() || !v->loadCmuDict(path)) {
        warn("DialogJobManager: CMU dict not loaded (path='{}'); mouth cues will fall back to whatever TextToViseme "
             "does with empty data",
             path);
    }
    textToViseme_ = v;
    return textToViseme_;
}

Result<std::string> DialogJobManager::submitJob(std::vector<DialogJobTurn> turns, DialogPersistence persistence,
                                                bool autoplay, std::string title,
                                                std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability->createChildOperationSpan("DialogJobManager.submitJob", parentSpan);
    if (span) {
        span->setAttribute("dialog.turns", static_cast<int64_t>(turns.size()));
        span->setAttribute("dialog.persistence", persistence == DialogPersistence::AdHoc ? "adhoc" : "permanent");
        span->setAttribute("dialog.autoplay", autoplay);
    }

    if (turns.empty()) {
        return Result<std::string>{ServerError(ServerError::InvalidData, "submitJob: turns is empty")};
    }

    // ---- Resolve every UNIQUE creature in turns. We need the full creature
    // doc anyway (motors, mouth_slot, voice config) so cache it on the job.
    std::vector<DialogJobCreature> resolved;
    std::unordered_map<std::string, std::size_t> byCreatureId;
    for (const auto &turn : turns) {
        if (turn.creatureId.empty()) {
            return Result<std::string>{ServerError(ServerError::InvalidData, "submitJob: a turn has empty creatureId")};
        }
        if (byCreatureId.count(turn.creatureId)) {
            continue; // already resolved this creature on an earlier turn
        }
        auto jsonResult = creatures::db->getCreatureJson(turn.creatureId, span);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            return Result<std::string>{
                ServerError(ServerError::InvalidData, fmt::format("submitJob: creature '{}' lookup failed: {}",
                                                                  turn.creatureId, err.getMessage()))};
        }
        const auto cj = jsonResult.getValue().value();

        // Extract everything the job + worker will need. Fail fast on any
        // missing required field.
        if (!cj.contains("voice") || !cj["voice"].is_object()) {
            return Result<std::string>{
                ServerError(ServerError::InvalidData,
                            fmt::format("submitJob: creature '{}' has no voice config", turn.creatureId))};
        }
        const auto &voice = cj["voice"];
        if (!voice.contains("voice_id") || !voice["voice_id"].is_string()) {
            return Result<std::string>{
                ServerError(ServerError::InvalidData,
                            fmt::format("submitJob: creature '{}' has no voice.voice_id", turn.creatureId))};
        }
        if (!cj.contains("audio_channel") || !cj["audio_channel"].is_number()) {
            return Result<std::string>{
                ServerError(ServerError::InvalidData,
                            fmt::format("submitJob: creature '{}' has no audio_channel", turn.creatureId))};
        }
        if (!cj.contains("mouth_slot") || !cj["mouth_slot"].is_number()) {
            return Result<std::string>{ServerError(
                ServerError::InvalidData, fmt::format("submitJob: creature '{}' has no mouth_slot", turn.creatureId))};
        }

        DialogJobCreature c;
        c.creatureId = turn.creatureId;
        c.creatureJson = cj;
        c.voiceId = voice["voice_id"].get<std::string>();
        c.audioChannel = cj["audio_channel"].get<uint16_t>();
        c.mouthSlot = cj["mouth_slot"].get<uint8_t>();
        c.universe = 0;
        if (auto u = tryGetUniverse(turn.creatureId)) {
            c.universe = *u;
        }

        byCreatureId.emplace(turn.creatureId, resolved.size());
        resolved.push_back(std::move(c));
    }

    // ---- Cross-creature validation.
    // 1. Distinct audio_channels (silent data loss in the 17-ch interleave
    //    otherwise — DialogWav would catch it too, but better to fail fast).
    // 2. Distinct mouth_slots (one creature's mouth byte would clobber
    //    another's on the universe wire).
    // 3. ≤10 unique voice IDs per scene (ElevenLabs per-call cap).
    // 4. If autoplay: all creatures must resolve to the same universe.
    std::unordered_set<uint16_t> channels;
    std::unordered_set<uint8_t> mouthSlots;
    std::unordered_set<std::string> uniqueVoices;
    for (const auto &c : resolved) {
        if (!channels.insert(c.audioChannel).second) {
            return Result<std::string>{ServerError(
                ServerError::InvalidData,
                fmt::format("submitJob: audio_channel {} is assigned to more than one creature in this scene",
                            c.audioChannel))};
        }
        if (!mouthSlots.insert(c.mouthSlot).second) {
            return Result<std::string>{
                ServerError(ServerError::InvalidData,
                            fmt::format("submitJob: mouth_slot {} is assigned to more than one creature in this scene",
                                        c.mouthSlot))};
        }
        uniqueVoices.insert(c.voiceId);
    }
    if (uniqueVoices.size() > kMaxUniqueVoicesPerScene) {
        return Result<std::string>{
            ServerError(ServerError::InvalidData, fmt::format("submitJob: {} unique voices exceeds per-scene cap of {}",
                                                              uniqueVoices.size(), kMaxUniqueVoicesPerScene))};
    }
    if (autoplay) {
        std::optional<universe_t> commonUniverse;
        for (const auto &c : resolved) {
            auto u = tryGetUniverse(c.creatureId);
            if (!u) {
                return Result<std::string>{ServerError(
                    ServerError::InvalidData,
                    fmt::format("submitJob: autoplay requested but creature '{}' is not registered with a universe",
                                c.creatureId))};
            }
            if (!commonUniverse) {
                commonUniverse = u;
            } else if (*commonUniverse != *u) {
                return Result<std::string>{
                    ServerError(ServerError::InvalidData,
                                fmt::format("submitJob: autoplay requires all creatures on one universe ({} != {})",
                                            static_cast<long long>(*commonUniverse), static_cast<long long>(*u)))};
            }
        }
    }

    // ---- Register the job, kick off the worker.
    auto job = std::make_shared<DialogJob>();
    job->id = util::generateUUID();
    job->turns = std::move(turns);
    job->creatures = std::move(resolved);
    job->persistence = persistence;
    job->autoplay = autoplay;
    job->title = title.empty() ? fmt::format("Dialog {}", job->id) : std::move(title);

    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        jobs_.emplace(job->id, job);
    }

    if (span) {
        span->setAttribute("dialog.job_id", job->id);
        span->setAttribute("dialog.unique_creatures", static_cast<int64_t>(job->creatures.size()));
        span->setAttribute("dialog.unique_voices", static_cast<int64_t>(uniqueVoices.size()));
        span->setSuccess();
    }

    info("dialog job {} submitted: {} turns, {} unique creatures, persistence={}, autoplay={}", job->id,
         job->turns.size(), job->creatures.size(), persistence == DialogPersistence::AdHoc ? "adhoc" : "permanent",
         autoplay);

    // Detached worker. The job is owned by the registry; the worker copy keeps
    // it alive for the duration of the run even if a future cleanup pass were
    // to expire the registry entry.
    auto jobSpan = creatures::observability->createOperationSpan("DialogJob.run", parentSpan);
    std::thread([this, job, jobSpan]() { runJob(job, jobSpan); }).detach();

    return job->id;
}

std::optional<DialogJobStatusSnapshot> DialogJobManager::getStatus(const std::string &jobId) {
    std::shared_ptr<DialogJob> job;
    {
        std::lock_guard<std::mutex> lock(registryMutex_);
        auto it = jobs_.find(jobId);
        if (it == jobs_.end()) {
            return std::nullopt;
        }
        job = it->second;
    }
    DialogJobStatusSnapshot snap;
    snap.id = jobId;
    std::lock_guard<std::mutex> jlock(job->statusMutex);
    snap.status = job->status;
    snap.animationId = job->animationId;
    snap.errorMessage = job->errorMessage;
    return snap;
}

void DialogJobManager::runJob(std::shared_ptr<DialogJob> job, std::shared_ptr<OperationSpan> jobSpan) {
    if (jobSpan) {
        jobSpan->setAttribute("dialog.job_id", job->id);
    }
    {
        std::lock_guard<std::mutex> lock(job->statusMutex);
        job->status = DialogJobStatus::Running;
    }

    // failJob: stamp the error message, mark Failed, set span error, return.
    auto failJob = [&](const std::string &msg) {
        error("dialog job {}: {}", job->id, msg);
        if (jobSpan) {
            jobSpan->setError(msg);
        }
        std::lock_guard<std::mutex> lock(job->statusMutex);
        job->status = DialogJobStatus::Failed;
        job->errorMessage = msg;
    };

    try {
        // Index per-creature lookup by creatureId. Build the DialogInput list
        // from turns, looking each creature up to get its voice_id.
        std::unordered_map<std::string, const DialogJobCreature *> byCreatureId;
        for (const auto &c : job->creatures) {
            byCreatureId[c.creatureId] = &c;
        }
        std::vector<DialogInput> inputs;
        inputs.reserve(job->turns.size());
        for (const auto &t : job->turns) {
            auto it = byCreatureId.find(t.creatureId);
            if (it == byCreatureId.end()) {
                return failJob(fmt::format("creature '{}' missing from job cache (internal error)", t.creatureId));
            }
            inputs.push_back({it->second->voiceId, t.text});
        }

        // ---- Chunk + per-chunk generate/align/assemble.
        auto chunksResult = chunkTurns(inputs);
        if (!chunksResult.isSuccess()) {
            return failJob(chunksResult.getError().value().getMessage());
        }
        const auto chunks = chunksResult.getValue().value();
        if (jobSpan) {
            jobSpan->setAttribute("dialog.chunks", static_cast<int64_t>(chunks.size()));
        }

        DialogClient client;
        const std::string apiKey = creatures::config->getVoiceApiKey();

        std::vector<DialogAssembled> assembledChunks;
        assembledChunks.reserve(chunks.size());
        for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
            const auto &chunk = chunks[ci];
            auto chunkSpan =
                creatures::observability->createChildOperationSpan(fmt::format("DialogJob.chunk.{}", ci), jobSpan);

            // 1) Text-to-Dialogue (eleven_v3, pcm_48000).
            auto dialogResult = client.generateDialog(apiKey, chunk, "pcm_48000", chunkSpan);
            if (!dialogResult.isSuccess()) {
                return failJob(
                    fmt::format("chunk {} generateDialog: {}", ci, dialogResult.getError().value().getMessage()));
            }
            const auto dialog = dialogResult.getValue().value();

            // 2) Build the tag-stripped transcript and forced-align against
            //    the WAV-wrapped PCM. show.py's exact recipe.
            std::string transcript;
            for (std::size_t t = 0; t < chunk.size(); ++t) {
                if (t > 0) {
                    transcript.push_back(' ');
                }
                transcript += DialogClient::stripTags(chunk[t].text);
            }
            const auto wavBytes = wrapMonoPcmAsWav(dialog.audioData, kDialogSampleRate);
            auto alignResult = client.forcedAlignment(apiKey, wavBytes, "audio/wav", transcript, chunkSpan);
            if (!alignResult.isSuccess()) {
                return failJob(
                    fmt::format("chunk {} forcedAlignment: {}", ci, alignResult.getError().value().getMessage()));
            }
            const auto alignment = alignResult.getValue().value();

            // 3) Per-creature slice + mouth timing on the tightened timeline.
            auto assembledResult = assembleChunk(chunk, dialog, alignment, kDialogSampleRate);
            if (!assembledResult.isSuccess()) {
                return failJob(
                    fmt::format("chunk {} assembleChunk: {}", ci, assembledResult.getError().value().getMessage()));
            }
            assembledChunks.push_back(assembledResult.getValue().value());
        }

        auto concatResult = concatChunks(assembledChunks);
        if (!concatResult.isSuccess()) {
            return failJob(concatResult.getError().value().getMessage());
        }
        const auto assembled = concatResult.getValue().value();

        // ---- 17-channel WAV output.
        std::error_code ec;
        std::filesystem::create_directories(kDialogWavDir, ec);
        if (ec) {
            return failJob(fmt::format("create_directories({}) failed: {}", kDialogWavDir.string(), ec.message()));
        }
        const std::filesystem::path wavPath = kDialogWavDir / fmt::format("{}.wav", job->id);

        VoiceChannelMap voiceToChannel;
        for (const auto &c : job->creatures) {
            voiceToChannel.emplace(c.voiceId, c.audioChannel);
        }
        auto wavResult = writeDialogWav(assembled, voiceToChannel, wavPath, jobSpan);
        if (!wavResult.isSuccess()) {
            return failJob(wavResult.getError().value().getMessage());
        }

        // ---- Per-creature base body motion + mouth bytes.
        //
        // Pick a speech_loop_animation_ids entry per creature at random (same
        // policy as the ad-hoc path). Decode its frames. Render mouth bytes
        // from the CharTiming list this creature accumulated across chunks.
        auto viseme = getTextToViseme();
        SoundDataProcessor soundProc;

        // msPerFrame: pulled from the FIRST chosen base animation; we require
        // all subsequent ones to match. Multi-rate scenes don't make sense
        // here because everything plays on one shared timeline.
        std::optional<uint32_t> msPerFrame;
        std::size_t totalFrames = 0;

        std::vector<CreatureTrackInput> creatureInputs;
        creatureInputs.reserve(assembled.perCreature.size());

        std::mt19937 rng(static_cast<uint32_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));

        for (const auto &pc : assembled.perCreature) {
            // Find this voice's creature in the cache.
            const DialogJobCreature *cinfo = nullptr;
            for (const auto &c : job->creatures) {
                if (c.voiceId == pc.voiceId) {
                    cinfo = &c;
                    break;
                }
            }
            if (!cinfo) {
                return failJob(
                    fmt::format("post-assembly: voice '{}' has no matching creature in job cache", pc.voiceId));
            }

            // speech_loop_animation_ids — pick one at random.
            if (!cinfo->creatureJson.contains("speech_loop_animation_ids") ||
                !cinfo->creatureJson["speech_loop_animation_ids"].is_array() ||
                cinfo->creatureJson["speech_loop_animation_ids"].empty()) {
                return failJob(fmt::format("creature '{}' has no speech_loop_animation_ids", cinfo->creatureId));
            }
            const auto loopIds = cinfo->creatureJson["speech_loop_animation_ids"].get<std::vector<std::string>>();
            std::uniform_int_distribution<std::size_t> dist(0, loopIds.size() - 1);
            const auto chosenId = loopIds[dist(rng)];

            auto baseAnimResult = creatures::db->getAnimation(chosenId, jobSpan);
            if (!baseAnimResult.isSuccess()) {
                return failJob(fmt::format("creature '{}': load base anim {}: {}", cinfo->creatureId, chosenId,
                                           baseAnimResult.getError().value().getMessage()));
            }
            const auto baseAnim = baseAnimResult.getValue().value();

            // msPerFrame consistency check.
            if (!msPerFrame) {
                msPerFrame = baseAnim.metadata.milliseconds_per_frame;
                if (*msPerFrame == 0) {
                    *msPerFrame = 1; // mirror ad-hoc fallback; avoids divide-by-zero downstream
                }
                const double totalMs =
                    static_cast<double>(assembled.totalSamples) * 1000.0 / static_cast<double>(assembled.sampleRate);
                totalFrames = static_cast<std::size_t>(std::ceil(totalMs / static_cast<double>(*msPerFrame)));
            } else if (baseAnim.metadata.milliseconds_per_frame != *msPerFrame) {
                return failJob(fmt::format(
                    "creature '{}': base anim ms/frame {} differs from scene's {}; multi-rate dialog not supported",
                    cinfo->creatureId, baseAnim.metadata.milliseconds_per_frame, *msPerFrame));
            }

            // Find this creature's track in the base animation. Multi-creature
            // base anims exist (a track per creature); pick the one for THIS
            // creature.
            auto trackIt = std::find_if(baseAnim.tracks.begin(), baseAnim.tracks.end(),
                                        [&](const Track &t) { return t.creature_id == cinfo->creatureId; });
            if (trackIt == baseAnim.tracks.end()) {
                return failJob(fmt::format("creature '{}': base anim {} has no track for this creature",
                                           cinfo->creatureId, chosenId));
            }

            std::vector<std::vector<uint8_t>> baseFrames;
            baseFrames.reserve(trackIt->frames.size());
            for (const auto &f : trackIt->frames) {
                baseFrames.push_back(decodeBase64(f));
            }
            if (baseFrames.empty()) {
                return failJob(
                    fmt::format("creature '{}': base anim {} track has zero frames", cinfo->creatureId, chosenId));
            }

            // Render mouth bytes: CharTiming → RhubarbMouthCue → per-frame
            // servo bytes. SoundDataProcessor outputs targetFrameCount bytes
            // regardless of how many cues — silence frames come back as 0 (X).
            RhubarbSoundData snd;
            snd.metadata.duration =
                static_cast<double>(assembled.totalSamples) / static_cast<double>(assembled.sampleRate);
            snd.metadata.soundFile = wavPath.filename().string();
            snd.mouthCues = viseme->charTimingsToMouthCues(pc.mouth);
            auto mouthBytes = soundProc.processSoundData(snd, *msPerFrame, totalFrames);

            CreatureTrackInput cti;
            cti.voiceId = pc.voiceId;
            cti.creatureId = cinfo->creatureId;
            cti.creatureJson = cinfo->creatureJson;
            cti.baseFrames = std::move(baseFrames);
            cti.mouthBytes = std::move(mouthBytes);
            creatureInputs.push_back(std::move(cti));
        }

        if (!msPerFrame) {
            return failJob("post-assembly: msPerFrame not set (no creatures had usable base animations)");
        }

        // ---- Build the multi-track Animation.
        auto animResult =
            buildDialogAnimation(assembled, creatureInputs, *msPerFrame, wavPath.string(), job->title, jobSpan);
        if (!animResult.isSuccess()) {
            return failJob(animResult.getError().value().getMessage());
        }
        const auto animation = animResult.getValue().value();

        // ---- Persist.
        if (job->persistence == DialogPersistence::AdHoc) {
            auto insertResult =
                creatures::db->insertAdHocAnimation(animation, std::chrono::system_clock::now(), jobSpan);
            if (!insertResult.isSuccess()) {
                return failJob(fmt::format("insertAdHocAnimation: {}", insertResult.getError().value().getMessage()));
            }
        } else {
            // Permanent: serialize and upsert.
            const auto j = animationToJson(animation);
            auto upsertResult = creatures::db->upsertAnimation(j.dump(), jobSpan);
            if (!upsertResult.isSuccess()) {
                return failJob(fmt::format("upsertAnimation: {}", upsertResult.getError().value().getMessage()));
            }
        }

        // ---- Optional autoplay. Universe was validated to be common across
        // all creatures at submit time, so pull it from the first.
        if (job->autoplay && !job->creatures.empty()) {
            const auto universe = job->creatures.front().universe;
            // interrupt() expects a RequestSpan parent; the worker only has
            // an OperationSpan, so just pass nullptr (ad-hoc path does the same).
            auto interruptResult = creatures::sessionManager->interrupt(universe, animation, false, nullptr);
            if (!interruptResult.isSuccess()) {
                // Persistence already succeeded — don't fail the whole job
                // just because the autoplay couldn't fire. Log it loudly.
                warn("dialog job {}: persisted as {} but autoplay interrupt() failed: {}", job->id, animation.id,
                     interruptResult.getError().value().getMessage());
            } else {
                info("dialog job {}: autoplay interrupted universe {} with animation {}", job->id, universe,
                     animation.id);
            }
        }

        // ---- Success.
        {
            std::lock_guard<std::mutex> lock(job->statusMutex);
            job->status = DialogJobStatus::Succeeded;
            job->animationId = animation.id;
        }
        info("dialog job {}: succeeded; animation_id={}", job->id, animation.id);
        if (jobSpan) {
            jobSpan->setAttribute("dialog.animation_id", animation.id);
            jobSpan->setSuccess();
        }
    } catch (const std::exception &e) {
        failJob(fmt::format("worker exception: {}", e.what()));
    } catch (...) {
        failJob("worker exception: unknown");
    }
}

} // namespace creatures::voice
