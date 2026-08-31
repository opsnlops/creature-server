#include "server/ws/service/DialogMusicService.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <fmt/format.h>

#include "model/DialogScript.h"
#include "server/audio/MonoWavDownmixer.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/script/DialogScriptMutationLock.h"
#include "server/storage/Storage.h"
#include "server/voice/DialogCache.h"
#include "server/voice/IxmlReader.h"
#include "server/voice/MusicClient.h"
#include "server/voice/MusicGenerationCache.h"
#include "server/voice/PcmWavWriter.h"
#include "util/Sha256.h"
#include "util/Slugify.h"
#include "util/helpers.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::ws {

namespace {

int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string nowIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

struct DialogMusicContext {
    std::vector<voice::DialogInput> inputs;
    std::unordered_map<std::string, std::string> names;
};

Result<DialogMusicContext> loadDialogMusicContext(const DialogScript &script,
                                                  const std::shared_ptr<OperationSpan> &parentSpan) {
    DialogMusicContext context;
    context.inputs.reserve(script.turns.size());
    std::unordered_map<std::string, std::string> voiceIds;
    for (const auto &turn : script.turns) {
        if (!voiceIds.contains(turn.creature_id)) {
            auto creature = creatures::db->getCreatureJson(turn.creature_id, parentSpan);
            if (!creature.isSuccess()) {
                return Result<DialogMusicContext>{creature.getError().value()};
            }
            const auto json = creature.getValue().value();
            if (!json.contains("voice") || !json["voice"].is_object() || !json["voice"].contains("voice_id") ||
                !json["voice"]["voice_id"].is_string()) {
                return Result<DialogMusicContext>{
                    ServerError(ServerError::InvalidData, "dialog creature has no voice_id")};
            }
            voiceIds.emplace(turn.creature_id, json["voice"]["voice_id"].get<std::string>());
            context.names.emplace(turn.creature_id, json.value("name", turn.creature_id));
        }
        context.inputs.push_back({voiceIds.at(turn.creature_id), turn.text});
    }
    return Result<DialogMusicContext>{std::move(context)};
}

} // namespace

Result<api::DialogMusicGenerationResult> DialogMusicService::generate(const api::DialogMusicRequest &request,
                                                                      std::shared_ptr<OperationSpan> parentSpan,
                                                                      const std::string &jobId) const {
    auto span = creatures::observability
                    ? creatures::observability->createChildOperationSpan("DialogMusicService.generate", parentSpan)
                    : nullptr;
    using GenerateResult = Result<api::DialogMusicGenerationResult>;
    const auto fail = [&span](ServerError error, const std::string &type = "DialogMusicGenerationError") {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return GenerateResult{std::move(error)};
    };
    const std::string &scriptId = request.scriptId;
    const std::string &cacheKey = request.dialogCacheKey;
    const std::string &dialogGenerationId = request.dialogGenerationId;
    const std::string &prompt = request.prompt;
    const std::string &mode = request.generationMode;
    const int64_t durationExtensionMs = request.durationExtensionMs;
    if (span) {
        span->setAttribute("job.id", jobId);
        span->setAttribute("dialog.script_id", scriptId);
        span->setAttribute("dialog.generation_id", dialogGenerationId);
        span->setAttribute("dialog.cache_key", cacheKey);
        span->setAttribute("music.generation_mode", mode);
        span->setAttribute("music.prompt_length", static_cast<int64_t>(prompt.size()));
        span->setAttribute("music.duration_extension_ms", durationExtensionMs);
    }
    if (!isUuidShape(scriptId) || !isUuidShape(dialogGenerationId) || cacheKey.size() != 64 ||
        !std::all_of(cacheKey.begin(), cacheKey.end(),
                     [](unsigned char c) { return std::isxdigit(c) && !std::isupper(c); }) ||
        prompt.empty() || prompt.size() > voice::kMaxMusicPromptBytes || durationExtensionMs < 0 ||
        durationExtensionMs > voice::kMaxMusicDurationExtensionMs ||
        (mode != "track" && mode != "loop" && mode != "ambience")) {
        return fail(ServerError(ServerError::InvalidData, "dialog music request failed validation"), "InvalidData");
    }
    if (!creatures::db || !creatures::config) {
        return fail(ServerError(ServerError::InternalError, "dialog music dependencies are unavailable"),
                    "DependencyUnavailable");
    }

    auto scriptResult = creatures::db->getDialogScript(scriptId, span);
    if (!scriptResult.isSuccess()) {
        return fail(scriptResult.getError().value(), "DialogScriptLookupError");
    }
    const auto script = scriptResult.getValue().value();

    auto contextResult = loadDialogMusicContext(script, span);
    if (!contextResult.isSuccess()) {
        return fail(contextResult.getError().value(), "DialogContextLoadError");
    }
    auto context = contextResult.getValue().value();
    if (voice::computeCacheKey(context.inputs) != cacheKey) {
        return fail(ServerError(ServerError::InvalidData, "dialog cache key does not match the current saved script"),
                    "DialogCacheMismatch");
    }
    auto dialogGeneration = voice::loadGeneration(cacheKey, dialogGenerationId);
    if (!dialogGeneration.isSuccess()) {
        return fail(dialogGeneration.getError().value(), "DialogGenerationLoadError");
    }
    const auto dialogPcmBytes = dialogGeneration.getValue().value().audioPcm.size();
    const auto exactLengthMs = static_cast<int64_t>((dialogPcmBytes / sizeof(int16_t)) * 1000ULL / 48000ULL);
    const auto requestLengthMs = std::max<int64_t>(voice::kMinMusicLengthMs, exactLengthMs + durationExtensionMs);
    if (requestLengthMs > voice::kMaxMusicLengthMs) {
        return fail(ServerError(ServerError::InvalidData, "dialog is longer than ElevenLabs Music's 10-minute limit"),
                    "InvalidData");
    }

    const auto generationId = util::generateUUID();
    if (span) {
        span->setAttribute("music.generation_id", generationId);
        span->setAttribute("music.model_id", "music_v2");
        span->setAttribute("music.output_format", "pcm_48000");
        span->setAttribute("dialog.duration_ms", exactLengthMs);
        span->setAttribute("music.duration_extension_ms", durationExtensionMs);
        span->setAttribute("music.length_ms", requestLengthMs);
    }
    voice::MusicClient client;
    auto generated = client.generateInstrumental(
        creatures::config->getVoiceApiKey(), prompt, requestLengthMs, mode, span,
        {jobId, generationId, dialogGenerationId, scriptId, exactLengthMs, durationExtensionMs});
    if (!generated.isSuccess()) {
        return fail(generated.getError().value(), "ElevenLabsGenerationError");
    }
    auto music = generated.getValue().value();
    voice::WavProvenance provenance;
    provenance.fileUid = generationId;
    provenance.take = generationId;
    provenance.circled = false;
    provenance.sourceScriptId = script.id;
    provenance.title = script.title;
    provenance.generationIds = {generationId};
    provenance.tracks = {{1, "BGM"}};
    for (const auto &turn : script.turns) {
        provenance.script.push_back({context.names.at(turn.creature_id), turn.text});
    }
    voice::MusicWavProvenance recipe;
    recipe.provider = "ElevenLabs";
    recipe.endpoint = "POST /v1/music/detailed?output_format=pcm_48000";
    recipe.modelId = "music_v2";
    recipe.outputFormat = "pcm_48000";
    recipe.sourceChannels = music.sourceChannels;
    recipe.channelTransform = music.sourceChannels == 2 ? "stereo_to_mono_average" : "none";
    recipe.generationMode = mode;
    recipe.prompt = prompt;
    recipe.sourceDialogDurationMs = exactLengthMs;
    recipe.durationExtensionMs = durationExtensionMs;
    recipe.musicLengthMs = requestLengthMs;
    recipe.forceInstrumental = true;
    recipe.requestJson = music.request.dump();
    recipe.responseMetadataJson = music.responseMetadata.dump();
    recipe.compositionPlanJson = music.compositionPlan.dump();
    recipe.songMetadataJson = music.songMetadata.dump();
    recipe.songId = music.songId;
    recipe.requestId = music.requestId;
    recipe.generatedAt = nowIso8601();
    recipe.musicGenerationId = generationId;
    recipe.sourceDialogGenerationId = dialogGenerationId;
    recipe.sourceDialogCacheKey = cacheKey;
    recipe.sourceScriptUpdatedAt = script.updated_at;
    recipe.pcmSha256 = util::sha256Hex(std::span<const uint8_t>(music.audioPcm));
    provenance.music = std::move(recipe);

    voice::CachedMusicGeneration candidate;
    candidate.generationId = generationId;
    candidate.scriptId = script.id;
    candidate.title = script.title;
    candidate.prompt = prompt;
    candidate.generationMode = mode;
    candidate.durationSeconds = static_cast<double>(music.audioPcm.size() / sizeof(int16_t)) / 48000.0;
    candidate.provenance = std::move(provenance);
    auto saved = voice::saveMusicGeneration(candidate, music.audioPcm);
    if (!saved.isSuccess()) {
        return fail(saved.getError().value(), "MusicCacheWriteError");
    }

    api::DialogMusicGenerationResult result{
        generationId,
        fmt::format("/api/v1/animation/dialog/music/generated/{}.mp3", generationId),
        candidate.durationSeconds,
        exactLengthMs,
        durationExtensionMs,
        requestLengthMs,
        prompt};
    if (span) {
        span->setAttribute("music.generation_id", generationId);
        span->setAttribute("music.pcm_bytes", static_cast<int64_t>(music.audioPcm.size()));
        span->setSuccess();
    }
    return Result<api::DialogMusicGenerationResult>{std::move(result)};
}

Result<api::DialogMusicPromotionResult>
DialogMusicService::backfillMusicSourceFromPromotedFile(const std::string &generationId,
                                                        const std::shared_ptr<OperationSpan> &span) const {
    using PromotionResult = Result<api::DialogMusicPromotionResult>;
    const auto notFound = [](const std::string &message) {
        return PromotionResult{ServerError(ServerError::NotFound, message)};
    };

    auto scriptsResult = creatures::db->listDialogScripts(span);
    if (!scriptsResult.isSuccess()) {
        return PromotionResult{scriptsResult.getError().value()};
    }
    const auto scripts = scriptsResult.getValue().value();

    const auto match = std::find_if(scripts.begin(), scripts.end(), [&](const DialogScript &s) {
        return s.background_music && s.background_music->generation_id == generationId;
    });
    if (match == scripts.end()) {
        return notFound(fmt::format("no accepted music references generation {}", generationId));
    }
    auto script = *match;

    const auto promotedPath = storage::resolveSoundPath(script.background_music->sound_file);
    std::error_code ec;
    if (!std::filesystem::exists(promotedPath, ec) || ec) {
        return PromotionResult{
            ServerError(ServerError::NotFound,
                        fmt::format("script '{}' references music at '{}', which is missing from the permanent store",
                                    script.title, script.background_music->sound_file))};
    }

    const auto ixml = voice::readIxmlChunk(promotedPath);
    const auto provenance = ixml ? voice::parseIxmlProvenance(*ixml) : voice::WavProvenance{};
    if (!provenance.music || provenance.music->musicGenerationId != generationId) {
        return PromotionResult{ServerError(ServerError::InvalidData,
                                           fmt::format("the promoted music for script '{}' does not carry embedded "
                                                       "provenance for generation {}, so its composition source "
                                                       "cannot be recovered",
                                                       script.title, generationId))};
    }

    const auto buildResult = [&] {
        return api::DialogMusicPromotionResult{generationId, script.background_music->sound_file,
                                               fmt::format("/api/v1/sound/mp3/{}.mp3", promotedPath.stem().string())};
    };

    // Nothing to repair is still a success — this path is reached by re-running
    // an accept that already holds, and it should be as idempotent as the
    // ordinary one.
    if (!script.background_music->source_dialog_generation_id.empty() ||
        provenance.music->sourceDialogGenerationId.empty()) {
        if (span) {
            span->setAttribute("music.source_backfilled", false);
            span->setAttribute("music.recovered_from_promoted_file", true);
            span->setSuccess();
        }
        return PromotionResult{buildResult()};
    }

    script.background_music->source_dialog_generation_id = provenance.music->sourceDialogGenerationId;
    script.background_music->source_dialog_cache_key = provenance.music->sourceDialogCacheKey;
    script.updated_at = nowMillis();
    auto published = storage::publishDialogScript(dialogScriptToJson(script).dump(), span);
    if (!published.isSuccess()) {
        return PromotionResult{published.getError().value()};
    }

    info("recovered music composition source for script '{}' from the promoted WAV (generation {} has aged out of "
         "the candidate cache)",
         script.title, generationId);
    if (span) {
        span->setAttribute("music.source_backfilled", true);
        span->setAttribute("music.recovered_from_promoted_file", true);
        span->setSuccess();
    }
    return PromotionResult{buildResult()};
}

Result<api::DialogMusicPromotionResult> DialogMusicService::promote(const std::string &generationId,
                                                                    std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DialogMusicService.promote", parentSpan)
                    : nullptr;
    using PromotionResult = Result<api::DialogMusicPromotionResult>;
    const auto fail = [&span](ServerError error, const std::string &type = "DialogMusicPromotionError") {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return PromotionResult{std::move(error)};
    };
    if (span) {
        span->setAttribute("music.generation_id", generationId);
    }
    const std::scoped_lock mutationLock(creatures::script::mutationMutex());
    if (!creatures::db) {
        return fail(ServerError(ServerError::InternalError, "dialog music database is unavailable"),
                    "DependencyUnavailable");
    }
    auto loaded = voice::loadMusicGeneration(generationId);
    if (!loaded.isSuccess()) {
        // The candidate aged out of the TTL'd cache, but promotion COPIES —
        // the permanent WAV is still there, still carrying the same embedded
        // provenance. That's exactly the state the oldest accepted music is
        // in, which is to say precisely the music that most needs the #136
        // backfill. Repair from the promoted file instead of refusing.
        auto repaired = backfillMusicSourceFromPromotedFile(generationId, span);
        if (repaired.isSuccess()) {
            return repaired;
        }
        if (repaired.getError().value().getCode() != ServerError::NotFound) {
            return fail(repaired.getError().value(), "MusicSourceBackfillError");
        }
        return fail(loaded.getError().value(), "MusicCacheLoadError");
    }
    auto candidate = loaded.getValue().value();
    if (span) {
        span->setAttribute("dialog.script_id", candidate.scriptId);
    }
    auto scriptResult = creatures::db->getDialogScript(candidate.scriptId, span);
    if (!scriptResult.isSuccess()) {
        return fail(scriptResult.getError().value(), "DialogScriptLookupError");
    }
    auto script = scriptResult.getValue().value();

    // Promotion is safe to retry. Once this exact generation is attached and
    // the accepted asset still carries matching embedded provenance, return the
    // stable URLs without decoding or rewriting the WAV again.
    if (script.background_music && script.background_music->generation_id == generationId) {
        const auto existingPath = storage::resolveSoundPath(script.background_music->sound_file);
        const auto existingIxml = voice::readIxmlChunk(existingPath);
        const auto existingProvenance =
            existingIxml ? voice::parseIxmlProvenance(*existingIxml) : voice::WavProvenance{};
        if (existingProvenance.music && existingProvenance.music->musicGenerationId == generationId &&
            !existingProvenance.music->requestJson.empty()) {
            // Backfill the composition source if this music was accepted before
            // it was recorded (#136). The WAV has carried it all along, so
            // re-promoting the same generation repairs the script rather than
            // returning early with a block the Console can't render a verdict
            // for. Only writes when something actually changed.
            const auto &existingMusic = *existingProvenance.music;
            if (script.background_music->source_dialog_generation_id.empty() &&
                !existingMusic.sourceDialogGenerationId.empty()) {
                script.background_music->source_dialog_generation_id = existingMusic.sourceDialogGenerationId;
                script.background_music->source_dialog_cache_key = existingMusic.sourceDialogCacheKey;
                script.updated_at = nowMillis();
                auto backfilled = storage::publishDialogScript(dialogScriptToJson(script).dump(), span);
                if (!backfilled.isSuccess()) {
                    return fail(backfilled.getError().value(), "DialogScriptPublishError");
                }
                info("backfilled music composition source for script {} from embedded provenance", script.id);
                if (span) {
                    span->setAttribute("music.source_backfilled", true);
                }
            }
            api::DialogMusicPromotionResult result{
                generationId, script.background_music->sound_file,
                fmt::format("/api/v1/sound/mp3/{}.mp3", existingPath.stem().string())};
            if (span) {
                span->setAttribute("promotion.idempotent_hit", true);
                span->setAttribute("sound.file_hash", util::sha256Hex(script.background_music->sound_file));
                span->setAttribute("sound.file_extension", existingPath.extension().string());
                span->setSuccess();
            }
            return PromotionResult{std::move(result)};
        }
    }

    auto sourceIxml = voice::readIxmlChunk(candidate.wavPath);
    if (!sourceIxml) {
        return fail(ServerError(ServerError::InvalidData, "music candidate has no embedded provenance"),
                    "MissingProvenance");
    }
    auto provenance = voice::parseIxmlProvenance(*sourceIxml);
    if (!provenance.music || provenance.music->musicGenerationId != generationId ||
        provenance.music->requestJson.empty() || provenance.sourceScriptId != candidate.scriptId ||
        candidate.provenance.music->requestJson != provenance.music->requestJson) {
        return fail(ServerError(ServerError::InvalidData, "music candidate provenance failed verification"),
                    "ProvenanceVerificationError");
    }
    if (provenance.music->sourceDialogCacheKey.empty() ||
        provenance.music->sourceScriptUpdatedAt != script.updated_at) {
        return fail(ServerError(ServerError::InvalidData,
                                "dialog changed after this music take was generated; generate a new take"),
                    "StaleDialogRevision");
    }
    auto currentContext = loadDialogMusicContext(script, span);
    if (!currentContext.isSuccess()) {
        return fail(currentContext.getError().value(), "DialogContextLoadError");
    }
    if (voice::computeCacheKey(currentContext.getValue().value().inputs) != provenance.music->sourceDialogCacheKey) {
        return fail(ServerError(ServerError::InvalidData,
                                "dialog voices changed after this music take was generated; generate a new take"),
                    "StaleDialogVoices");
    }
    if (span) {
        span->setAttribute("promotion.idempotent_hit", false);
    }
    provenance.sourceScriptId = script.id;
    provenance.title = script.title;
    provenance.fileUid = generationId;
    provenance.take = generationId;
    provenance.circled = true;

    auto publicationSpan =
        creatures::observability
            ? creatures::observability->createChildOperationSpan("DialogMusicService.publishAcceptedWav", span)
            : nullptr;
    if (publicationSpan) {
        publicationSpan->setAttribute("music.generation_id", generationId);
        publicationSpan->setAttribute("sound.source", "music_generation_cache");
        std::error_code fileSizeError;
        const auto sourceBytes = std::filesystem::file_size(candidate.wavPath, fileSizeError);
        if (!fileSizeError)
            publicationSpan->setAttribute("sound.input_bytes", static_cast<int64_t>(sourceBytes));
    }
    const auto failPublication = [&](ServerError error, const std::string &type) {
        recordSpanError(publicationSpan, error.getMessage(), type, error.getCode());
        return fail(std::move(error), type);
    };

    auto mono = audio::loadWavAsMono(candidate.wavPath.string());
    if (!mono.isSuccess() || mono.getValue().value().sampleRate != 48000) {
        return failPublication(ServerError(ServerError::InvalidData, "music candidate is not a 48 kHz PCM WAV"),
                               "InvalidAudioFormat");
    }
    const auto monoAudio = mono.getValue().value();
    const auto &samples = monoAudio.samples;
    std::vector<uint8_t> pcm(samples.size() * sizeof(int16_t));
    std::memcpy(pcm.data(), samples.data(), pcm.size());
    if (util::sha256Hex(std::span<const uint8_t>(pcm)) != provenance.music->pcmSha256) {
        return failPublication(
            ServerError(ServerError::InvalidData, "music candidate PCM checksum does not match provenance"),
            "ChecksumMismatch");
    }
    const auto acceptedWav = voice::wrapMonoPcmAsWav(pcm, 48000, &provenance);
    const auto filename = util::bgmExportBasename(script.title, provenance.music->prompt, generationId) + ".wav";
    auto write =
        storage::writeSoundFile(storage::Persistence::Permanent, filename, acceptedWav, std::string("dialog/music"));
    if (!write.isSuccess()) {
        return failPublication(write.getError().value(), "PermanentWavWriteError");
    }
    const auto permanentPath = write.getValue().value();
    const auto verifyXml = voice::readIxmlChunk(permanentPath.absolute);
    const auto verified = verifyXml ? voice::parseIxmlProvenance(*verifyXml) : voice::WavProvenance{};
    if (!verified.music || verified.music->musicGenerationId != generationId ||
        verified.music->requestJson != provenance.music->requestJson) {
        std::error_code ignored;
        std::filesystem::remove(permanentPath.absolute, ignored);
        return failPublication(
            ServerError(ServerError::InternalError, "promoted music provenance could not be read back"),
            "ProvenanceReadbackError");
    }
    if (publicationSpan) {
        publicationSpan->setAttribute("music.checksum_verified", true);
        publicationSpan->setAttribute("music.provenance_verified", true);
        publicationSpan->setAttribute("sound.output_bytes", static_cast<int64_t>(acceptedWav.size()));
        publicationSpan->setAttribute("sound.file_hash", util::sha256Hex(permanentPath.forMetadata));
        publicationSpan->setSuccess();
    }

    // Carry the composition source onto the accepted block (#136). It comes from
    // the candidate's embedded iXML, which this function has already checksum-
    // verified and read back off disk — a stronger source than the request that
    // started the generation, because it travels with the audio.
    script.background_music = DialogBackgroundMusic{permanentPath.forMetadata,
                                                    generationId,
                                                    provenance.music->prompt,
                                                    nowMillis(),
                                                    provenance.music->sourceDialogGenerationId,
                                                    provenance.music->sourceDialogCacheKey};
    script.updated_at = nowMillis();
    auto published = storage::publishDialogScript(dialogScriptToJson(script).dump(), span);
    if (!published.isSuccess()) {
        std::error_code ignored;
        std::filesystem::remove(permanentPath.absolute, ignored);
        return fail(published.getError().value(), "DialogScriptPublishError");
    }

    api::DialogMusicPromotionResult result{
        generationId, permanentPath.forMetadata,
        fmt::format("/api/v1/sound/mp3/{}.mp3", std::filesystem::path(filename).stem().string())};
    if (span) {
        span->setAttribute("sound.file_hash", util::sha256Hex(permanentPath.forMetadata));
        span->setAttribute("sound.file_extension", permanentPath.absolute.extension().string());
        span->setAttribute("music.pcm_samples", static_cast<int64_t>(samples.size()));
        span->setSuccess();
    }
    return Result<api::DialogMusicPromotionResult>{std::move(result)};
}

} // namespace creatures::ws
