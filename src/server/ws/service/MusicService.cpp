#include "server/ws/service/MusicService.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <variant>

#include <fmt/format.h>

#include "server/audio/MonoWavDownmixer.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/storage/Storage.h"
#include "server/voice/IxmlReader.h"
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

nlohmann::json parseStoredJson(const std::string &text) {
    if (text.empty()) {
        return nlohmann::json::object();
    }
    try {
        return nlohmann::json::parse(text);
    } catch (const std::exception &) {
        return nlohmann::json::object();
    }
}

/// Sections a candidate was composed from, or nothing for prompt/plan takes.
std::optional<std::vector<voice::MusicSection>> sectionsFromProvenance(const voice::MusicWavProvenance &music) {
    if (music.sectionsJson.empty()) {
        return std::nullopt;
    }
    auto parsed = musicSectionsFromJson(parseStoredJson(music.sectionsJson), "provenance.sections");
    if (!parsed.isSuccess()) {
        return std::nullopt;
    }
    return parsed.getValue().value();
}

std::shared_ptr<OperationSpan> childSpan(const char *name, const std::shared_ptr<OperationSpan> &parent) {
    return creatures::observability ? creatures::observability->createChildOperationSpan(name, parent) : nullptr;
}

std::shared_ptr<OperationSpan> requestChildSpan(const char *name, const std::shared_ptr<RequestSpan> &parent) {
    return creatures::observability ? creatures::observability->createChildOperationSpan(name, parent) : nullptr;
}

} // namespace

api::DialogMusicRecipe MusicService::recipeFromProvenance(const voice::MusicWavProvenance &music) {
    api::DialogMusicRecipe recipe;
    recipe.modelId = music.modelId;
    recipe.songId = music.songId;
    // Pre-#200 takes have no requestKind; they were all prompt mode.
    recipe.requestKind = music.requestKind.empty() ? "prompt" : music.requestKind;
    recipe.prompt = music.prompt;
    if (recipe.requestKind == "prompt") {
        recipe.generationMode = music.generationMode;
        recipe.forceInstrumental = music.forceInstrumental;
    }
    recipe.seed = music.seed;
    if (!music.finetuneId.empty()) {
        recipe.finetuneId = music.finetuneId;
        recipe.finetuneStrength = music.finetuneStrength;
    }
    recipe.storedForInpainting = music.storedForInpainting;
    recipe.compositionPlan = parseStoredJson(music.compositionPlanJson);
    recipe.songMetadata = parseStoredJson(music.songMetadataJson);
    if (!music.sectionsJson.empty()) {
        auto sections = parseStoredJson(music.sectionsJson);
        if (sections.is_array()) {
            recipe.sections = std::move(sections);
        }
    }
    recipe.pieceId = music.pieceId;
    recipe.baseVersionId = music.baseVersionId;
    return recipe;
}

Result<api::DialogMusicGenerationResult> MusicService::compose(const voice::MusicGenerationRequest &upstream,
                                                               const MusicComposeContext &context,
                                                               voice::MusicGenerationTraceContext traceContext,
                                                               const std::shared_ptr<OperationSpan> &span) const {
    using ComposeResult = Result<api::DialogMusicGenerationResult>;
    const auto fail = [&span](ServerError error, const std::string &type) {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return ComposeResult{std::move(error)};
    };
    if (!creatures::config) {
        return fail(ServerError(ServerError::InternalError, "music dependencies are unavailable"),
                    "DependencyUnavailable");
    }
    const bool planMode = upstream.isPlanMode();
    const int64_t requestLengthMs = planMode ? upstream.compositionPlan->totalDurationMs() : upstream.musicLengthMs;
    if (span) {
        span->setAttribute("music.generation_id", context.generationId);
        span->setAttribute("music.output_format", "pcm_48000");
        span->setAttribute("music.length_ms", requestLengthMs);
        if (!context.pieceId.empty())
            span->setAttribute("music.piece_id", context.pieceId);
        if (!context.baseVersionId.empty())
            span->setAttribute("music.base_version_id", context.baseVersionId);
    }
    voice::MusicClient client;
    auto generated = client.generate(creatures::config->getVoiceApiKey(), upstream, span, std::move(traceContext));
    if (!generated.isSuccess()) {
        return fail(generated.getError().value(), "ElevenLabsGenerationError");
    }
    auto music = generated.getValue().value();

    voice::WavProvenance provenance;
    provenance.fileUid = context.generationId;
    provenance.take = context.generationId;
    provenance.circled = false;
    provenance.sourceScriptId = context.scriptId;
    provenance.title = context.title;
    provenance.generationIds = {context.generationId};
    provenance.tracks = {{1, "BGM"}};
    provenance.script = context.scriptLines;

    voice::MusicWavProvenance recipe;
    recipe.provider = "ElevenLabs";
    recipe.endpoint = "POST /v1/music/detailed?output_format=pcm_48000";
    recipe.modelId = upstream.modelId;
    recipe.outputFormat = "pcm_48000";
    recipe.sourceChannels = music.sourceChannels;
    recipe.channelTransform = music.sourceChannels == 2 ? "stereo_to_mono_average" : "none";
    recipe.generationMode = planMode ? std::string{} : upstream.generationMode;
    recipe.prompt = upstream.prompt;
    recipe.sourceDialogDurationMs = context.sourceDialogDurationMs;
    recipe.durationExtensionMs = planMode ? 0 : context.durationExtensionMs;
    recipe.musicLengthMs = requestLengthMs;
    recipe.forceInstrumental = planMode ? false : upstream.forceInstrumental;
    // Sections mode is a composition plan upstream; the recipe remembers that
    // the sections, not the plan, are the editable truth.
    recipe.requestKind = context.sections ? "sections" : upstream.requestKind();
    recipe.seed = planMode ? upstream.seed : std::nullopt;
    recipe.finetuneId = upstream.finetuneId.value_or("");
    recipe.finetuneStrength =
        upstream.finetuneId ? std::optional<double>{upstream.finetuneStrength.value_or(1.0)} : std::nullopt;
    recipe.storedForInpainting = upstream.storeForInpainting;
    if (context.sections) {
        recipe.sectionsJson = voice::musicSectionsToJson(*context.sections).dump();
    }
    recipe.pieceId = context.pieceId;
    recipe.baseVersionId = context.baseVersionId;
    recipe.requestJson = music.request.dump();
    recipe.responseMetadataJson = music.responseMetadata.dump();
    recipe.compositionPlanJson = music.compositionPlan.dump();
    recipe.songMetadataJson = music.songMetadata.dump();
    recipe.songId = music.songId;
    recipe.requestId = music.requestId;
    recipe.generatedAt = nowIso8601();
    recipe.musicGenerationId = context.generationId;
    recipe.sourceDialogGenerationId = context.sourceDialogGenerationId;
    recipe.sourceDialogCacheKey = context.sourceDialogCacheKey;
    recipe.sourceScriptUpdatedAt = context.sourceScriptUpdatedAt;
    recipe.pcmSha256 = util::sha256Hex(std::span<const uint8_t>(music.audioPcm));
    provenance.music = std::move(recipe);

    voice::CachedMusicGeneration candidate;
    candidate.generationId = context.generationId;
    candidate.scriptId = context.scriptId;
    candidate.title = context.title;
    candidate.prompt = upstream.prompt;
    candidate.generationMode = planMode ? std::string{} : upstream.generationMode;
    candidate.durationSeconds = static_cast<double>(music.audioPcm.size() / sizeof(int16_t)) / 48000.0;
    candidate.provenance = std::move(provenance);
    const auto resultRecipe = recipeFromProvenance(*candidate.provenance.music);
    auto saved = voice::saveMusicGeneration(candidate, music.audioPcm);
    if (!saved.isSuccess()) {
        return fail(saved.getError().value(), "MusicCacheWriteError");
    }
    api::DialogMusicGenerationResult result{context.generationId,
                                            fmt::format("{}{}.mp3", context.mp3UrlPrefix, context.generationId),
                                            candidate.durationSeconds,
                                            context.sourceDialogDurationMs,
                                            planMode ? 0 : context.durationExtensionMs,
                                            requestLengthMs,
                                            upstream.prompt,
                                            resultRecipe};
    if (span) {
        span->setAttribute("music.pcm_bytes", static_cast<int64_t>(music.audioPcm.size()));
        span->setSuccess();
    }
    return ComposeResult{std::move(result)};
}

Result<MusicService::Prepared> MusicService::prepare(const api::MusicGenerateRequest &request,
                                                     const MusicPiece *piece) {
    using PrepareResult = Result<Prepared>;
    Prepared prepared;
    auto &context = prepared.context;
    auto &upstream = prepared.upstream;
    context.generationId = util::generateUUID();
    context.pieceId = request.pieceId;
    context.mp3UrlPrefix = "/api/v1/music/generated/";

    std::optional<voice::MusicBaseVersion> base;
    if (!request.pieceId.empty()) {
        if (!piece) {
            return PrepareResult{ServerError(ServerError::NotFound, "music piece not found")};
        }
        context.title = piece->title;
        if (!request.baseVersionId.empty()) {
            const auto *version = piece->findVersion(request.baseVersionId);
            if (!version) {
                return PrepareResult{
                    ServerError(ServerError::InvalidData,
                                fmt::format("piece {} has no version {}", request.pieceId, request.baseVersionId))};
            }
            if (version->song_id.empty()) {
                return PrepareResult{ServerError(ServerError::InvalidData,
                                                 "the base version has no ElevenLabs song id, so its sections cannot "
                                                 "be kept or conditioned on; generate without base_version_id")};
            }
            base = voice::MusicBaseVersion{version->song_id, version->sections};
            context.baseVersionId = request.baseVersionId;
        }
    }

    upstream.modelId = request.modelId;
    upstream.finetuneId = request.finetuneId;
    upstream.finetuneStrength = request.finetuneStrength;
    upstream.storeForInpainting = request.storeForInpainting;
    if (request.isSectionsMode()) {
        auto built = voice::buildMusicSectionsPlan(*request.sections, base, request.keep, request.conditionStrength);
        if (std::holds_alternative<voice::MusicSectionsPlanError>(built)) {
            return PrepareResult{
                ServerError(ServerError::InvalidData, std::get<voice::MusicSectionsPlanError>(built).message)};
        }
        upstream.compositionPlan = std::get<voice::MusicCompositionPlan>(built);
        upstream.seed = request.seed;
        context.sections = request.sections;
    } else if (request.isPlanMode()) {
        upstream.compositionPlan = request.compositionPlan;
        upstream.seed = request.seed;
    } else {
        upstream.prompt = request.prompt;
        upstream.musicLengthMs = request.musicLengthMs;
        upstream.generationMode = request.generationMode;
        upstream.forceInstrumental = request.forceInstrumental;
    }
    return PrepareResult{std::move(prepared)};
}

Result<api::DialogMusicGenerationResult> MusicService::generate(const api::MusicGenerateRequest &request,
                                                                std::shared_ptr<OperationSpan> parentSpan,
                                                                const std::string &jobId) const {
    auto span = childSpan("MusicService.generate", parentSpan);
    using GenerateResult = Result<api::DialogMusicGenerationResult>;
    const auto fail = [&span](ServerError error, const std::string &type = "MusicGenerationError") {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return GenerateResult{std::move(error)};
    };
    if (span) {
        span->setAttribute("job.id", jobId);
        span->setAttribute("music.request_kind", request.requestKind());
        span->setAttribute("music.model_id", request.modelId);
        if (!request.pieceId.empty())
            span->setAttribute("music.piece_id", request.pieceId);
    }
    if (!creatures::db || !creatures::config) {
        return fail(ServerError(ServerError::InternalError, "music dependencies are unavailable"),
                    "DependencyUnavailable");
    }
    // Copy the piece out of the Result: getValue() returns the optional by
    // value, and a pointer into that temporary dangled on prod (3.48.1).
    std::optional<MusicPiece> piece;
    if (!request.pieceId.empty()) {
        auto loaded = creatures::db->getMusicPiece(request.pieceId, span);
        if (!loaded.isSuccess()) {
            return fail(loaded.getError().value(), "MusicPieceLookupError");
        }
        piece = loaded.getValue().value();
    }
    auto prepared = prepare(request, piece ? &*piece : nullptr);
    if (!prepared.isSuccess()) {
        return fail(prepared.getError().value(), "MusicSectionsPlanError");
    }
    const auto ready = prepared.getValue().value();
    if (span && request.sections) {
        span->setAttribute("music.section_count", static_cast<int64_t>(request.sections->size()));
        span->setAttribute("music.kept_count", static_cast<int64_t>(request.keep.size()));
    }
    return compose(ready.upstream, ready.context, {jobId, ready.context.generationId, {}, {}, 0, 0}, span);
}

Result<storage::StoragePath> MusicService::publishCandidateWav(const voice::CachedMusicGeneration &candidate,
                                                               const voice::WavProvenance &provenance,
                                                               const std::string &filename, const std::string &subdir,
                                                               const std::shared_ptr<OperationSpan> &span) {
    using PublishResult = Result<storage::StoragePath>;
    const auto fail = [&span](ServerError error, const std::string &type) {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return PublishResult{std::move(error)};
    };
    if (!provenance.music) {
        return fail(ServerError(ServerError::InvalidData, "music candidate has no provenance"), "MissingProvenance");
    }
    if (span) {
        span->setAttribute("music.generation_id", candidate.generationId);
        span->setAttribute("sound.source", "music_generation_cache");
        std::error_code fileSizeError;
        const auto sourceBytes = std::filesystem::file_size(candidate.wavPath, fileSizeError);
        if (!fileSizeError)
            span->setAttribute("sound.input_bytes", static_cast<int64_t>(sourceBytes));
    }
    auto mono = audio::loadWavAsMono(candidate.wavPath.string());
    if (!mono.isSuccess() || mono.getValue().value().sampleRate != 48000) {
        return fail(ServerError(ServerError::InvalidData, "music candidate is not a 48 kHz PCM WAV"),
                    "InvalidAudioFormat");
    }
    const auto monoAudio = mono.getValue().value();
    const auto &samples = monoAudio.samples;
    std::vector<uint8_t> pcm(samples.size() * sizeof(int16_t));
    std::memcpy(pcm.data(), samples.data(), pcm.size());
    if (util::sha256Hex(std::span<const uint8_t>(pcm)) != provenance.music->pcmSha256) {
        return fail(ServerError(ServerError::InvalidData, "music candidate PCM checksum does not match provenance"),
                    "ChecksumMismatch");
    }
    const auto wav = voice::wrapMonoPcmAsWav(pcm, 48000, &provenance);
    auto write = storage::writeSoundFile(storage::Persistence::Permanent, filename, wav, subdir);
    if (!write.isSuccess()) {
        return fail(write.getError().value(), "PermanentWavWriteError");
    }
    const auto permanentPath = write.getValue().value();
    const auto verifyXml = voice::readIxmlChunk(permanentPath.absolute);
    const auto verified = verifyXml ? voice::parseIxmlProvenance(*verifyXml) : voice::WavProvenance{};
    if (!verified.music || verified.music->musicGenerationId != candidate.generationId ||
        verified.music->requestJson != provenance.music->requestJson) {
        std::error_code ignored;
        std::filesystem::remove(permanentPath.absolute, ignored);
        return fail(ServerError(ServerError::InternalError, "promoted music provenance could not be read back"),
                    "ProvenanceReadbackError");
    }
    if (span) {
        span->setAttribute("sound.file_hash", util::sha256Hex(permanentPath.forMetadata));
        span->setAttribute("sound.output_bytes", static_cast<int64_t>(wav.size()));
        span->setSuccess();
    }
    return PublishResult{permanentPath};
}

Result<MusicPiece> MusicService::save(const std::string &generationId, const api::MusicSaveRequest &request,
                                      bool &created, std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = requestChildSpan("MusicService.save", parentSpan);
    using SaveResult = Result<MusicPiece>;
    const auto fail = [&span](ServerError error, const std::string &type = "MusicSaveError") {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return SaveResult{std::move(error)};
    };
    created = false;
    if (span) {
        span->setAttribute("music.generation_id", generationId);
        if (!request.pieceId.empty())
            span->setAttribute("music.piece_id", request.pieceId);
    }
    if (!creatures::db) {
        return fail(ServerError(ServerError::InternalError, "music database is unavailable"), "DependencyUnavailable");
    }

    // Idempotent: a generation that is already a version of the named piece
    // returns the piece unchanged.
    std::optional<MusicPiece> existing;
    if (!request.pieceId.empty()) {
        auto piece = creatures::db->getMusicPiece(request.pieceId, span);
        if (!piece.isSuccess()) {
            return fail(piece.getError().value(), "MusicPieceLookupError");
        }
        existing = piece.getValue().value();
        if (existing->findVersion(generationId)) {
            if (span) {
                span->setAttribute("save.idempotent_hit", true);
                span->setSuccess();
            }
            return SaveResult{*existing};
        }
    }

    auto loaded = voice::loadMusicGeneration(generationId);
    if (!loaded.isSuccess()) {
        return fail(loaded.getError().value(), "MusicCacheLoadError");
    }
    auto candidate = loaded.getValue().value();
    const auto sourceIxml = voice::readIxmlChunk(candidate.wavPath);
    if (!sourceIxml) {
        return fail(ServerError(ServerError::InvalidData, "music candidate has no embedded provenance"),
                    "MissingProvenance");
    }
    auto provenance = voice::parseIxmlProvenance(*sourceIxml);
    if (!provenance.music || provenance.music->musicGenerationId != generationId ||
        provenance.music->requestJson.empty() ||
        candidate.provenance.music->requestJson != provenance.music->requestJson) {
        return fail(ServerError(ServerError::InvalidData, "music candidate provenance failed verification"),
                    "ProvenanceVerificationError");
    }
    if (!provenance.music->pieceId.empty() && !request.pieceId.empty() &&
        provenance.music->pieceId != request.pieceId) {
        return fail(ServerError(ServerError::InvalidData, fmt::format("this take was generated for piece {}, not {}",
                                                                      provenance.music->pieceId, request.pieceId)),
                    "MusicPieceMismatch");
    }

    // Sections: the request's, else the candidate's own, else a single
    // section standing in for a prompt/plan take so the piece is editable.
    std::vector<voice::MusicSection> sections;
    const auto durationMs = static_cast<int64_t>(std::llround(candidate.durationSeconds * 1000.0));
    if (request.sections) {
        sections = *request.sections;
    } else if (auto own = sectionsFromProvenance(*provenance.music)) {
        sections = *own;
    } else {
        voice::MusicSection whole;
        whole.text = provenance.music->prompt.empty() ? "[Piece]" : provenance.music->prompt;
        whole.durationMs =
            std::clamp<int64_t>(durationMs, voice::kMinMusicChunkDurationMs, voice::kMaxMusicChunkDurationMs);
        sections = {whole};
    }
    const auto sectionsTotal = voice::musicSectionsTotalMs(sections);
    // Allow a little slack: ElevenLabs returns a few ms over the plan.
    if (request.sections && std::llabs(sectionsTotal - durationMs) > 250) {
        return fail(ServerError(ServerError::InvalidData,
                                fmt::format("sections total {} ms but the take is {} ms; sections must describe "
                                            "the audio",
                                            sectionsTotal, durationMs)),
                    "MusicSectionsMismatch");
    }

    const auto now = nowMillis();
    MusicPiece piece;
    if (existing) {
        piece = *existing;
    } else {
        piece.id = util::generateUUID();
        piece.title = request.title;
        piece.notes = request.notes;
        piece.created_at = now;
        created = true;
    }
    if (!request.notes.empty() && existing) {
        piece.notes = request.notes;
    }

    provenance.fileUid = generationId;
    provenance.take = generationId;
    provenance.circled = true;
    provenance.title = piece.title;
    provenance.music->pieceId = piece.id;
    if (!provenance.music->sectionsJson.empty() || request.sections) {
        provenance.music->sectionsJson = voice::musicSectionsToJson(sections).dump();
    }
    const auto filename = util::musicExportBasename(piece.title, generationId) + ".wav";
    auto publishSpan = childSpan("MusicService.publishPieceWav", span);
    auto published = publishCandidateWav(candidate, provenance, filename, "music", publishSpan);
    if (!published.isSuccess()) {
        return fail(published.getError().value(), "PieceWavPublishError");
    }
    const auto permanentPath = published.getValue().value();

    MusicPieceVersion version;
    version.id = generationId;
    version.song_id = provenance.music->songId;
    version.sound_file = permanentPath.forMetadata;
    version.mp3_url = fmt::format("/api/v1/sound/mp3/{}.mp3", std::filesystem::path(filename).stem().string());
    version.duration_ms = durationMs;
    version.recipe = api::dialogMusicRecipeToJson(recipeFromProvenance(*provenance.music));
    version.sections = std::move(sections);
    version.base_version_id = provenance.music->baseVersionId;
    if (!provenance.music->sourceDialogGenerationId.empty()) {
        version.source_dialog =
            MusicVersionSourceDialog{provenance.sourceScriptId, provenance.music->sourceDialogCacheKey,
                                     provenance.music->sourceDialogGenerationId};
    }
    version.created_at = now;
    piece.versions.push_back(std::move(version));
    piece.current_version_id = generationId;
    piece.updated_at = now;

    auto stored = storage::publishMusicPiece(piece, span);
    if (!stored.isSuccess()) {
        std::error_code removeError;
        const bool removed = std::filesystem::remove(permanentPath.absolute, removeError);
        if (span) {
            span->setAttribute("save.rolled_back", removed && !removeError);
        }
        return fail(stored.getError().value(), "MusicPiecePublishError");
    }
    if (span) {
        span->setAttribute("music.piece_id", piece.id);
        span->setAttribute("music.version_count", static_cast<int64_t>(piece.versions.size()));
        span->setAttribute("save.created_piece", created);
        span->setSuccess();
    }
    return stored;
}

namespace {

/// ElevenLabs' planner output → editable sections, or why it can't be.
Result<std::vector<voice::MusicSection>> sectionsFromPlan(const nlohmann::json &plan) {
    using ListResult = Result<std::vector<voice::MusicSection>>;
    if (!plan.is_object() || !plan.contains("chunks") || !plan["chunks"].is_array()) {
        return ListResult{ServerError(ServerError::InternalError, "ElevenLabs returned a plan without chunks")};
    }
    auto sections = nlohmann::json::array();
    for (const auto &chunk : plan["chunks"]) {
        auto section = voice::planChunkToSectionJson(chunk);
        if (section.is_null()) {
            return ListResult{
                ServerError(ServerError::InternalError, "ElevenLabs returned a plan chunk with no editable content")};
        }
        sections.push_back(std::move(section));
    }
    auto parsed = musicSectionsFromJson(sections, "ElevenLabs plan");
    if (!parsed.isSuccess()) {
        // Their limits and ours are the same document; a violation here is
        // upstream misbehaviour, not a client error.
        return ListResult{ServerError(ServerError::InternalError, parsed.getError().value().getMessage())};
    }
    return parsed;
}

} // namespace

Result<api::MusicRefineResult> MusicService::refine(const std::string &pieceId, const api::MusicRefineRequest &request,
                                                    std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = requestChildSpan("MusicService.refine", parentSpan);
    using RefineResult = Result<api::MusicRefineResult>;
    const auto fail = [&span](ServerError error, const std::string &type) {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return RefineResult{std::move(error)};
    };
    if (span) {
        span->setAttribute("music.piece_id", pieceId);
        span->setAttribute("music.instruction_length", static_cast<int64_t>(request.instruction.size()));
    }
    if (!creatures::db || !creatures::config) {
        return fail(ServerError(ServerError::InternalError, "music dependencies are unavailable"),
                    "DependencyUnavailable");
    }
    auto loaded = creatures::db->getMusicPiece(pieceId, span);
    if (!loaded.isSuccess()) {
        return fail(loaded.getError().value(), "MusicPieceLookupError");
    }
    const auto piece = loaded.getValue().value();
    const auto *base = request.versionId.empty() ? piece.currentVersion() : piece.findVersion(request.versionId);
    if (!base) {
        return fail(ServerError(ServerError::InvalidData,
                                fmt::format("piece {} has no version {}", pieceId,
                                            request.versionId.empty() ? piece.current_version_id : request.versionId)),
                    "MusicVersionNotFound");
    }
    const auto lengthMs = voice::musicSectionsTotalMs(base->sections);
    const auto modelId = base->recipe.value("model_id", std::string(voice::kDefaultMusicModelId));
    if (span) {
        span->setAttribute("music.base_version_id", base->id);
        span->setAttribute("music.model_id", modelId);
        span->setAttribute("music.length_ms", lengthMs);
    }
    voice::MusicClient client;
    nlohmann::json source{{"chunks", voice::musicSectionsToJson(base->sections)}};
    auto planned =
        client.generatePlan(creatures::config->getVoiceApiKey(), request.instruction, lengthMs, modelId, source, span);
    if (!planned.isSuccess()) {
        return fail(planned.getError().value(), "ElevenLabsPlanError");
    }
    auto sections = sectionsFromPlan(planned.getValue()->compositionPlan);
    if (!sections.isSuccess()) {
        return fail(sections.getError().value(), "PlanNormalisationError");
    }
    api::MusicRefineResult result;
    result.baseVersionId = base->id;
    result.modelId = modelId;
    result.musicLengthMs = voice::musicSectionsTotalMs(sections.getValue().value());
    result.sections = sections.getValue().value();
    result.diff = voice::diffMusicSections(base->sections, result.sections);
    result.compositionPlan = planned.getValue()->compositionPlan;
    if (span) {
        span->setAttribute("music.section_count", static_cast<int64_t>(result.sections.size()));
        span->setAttribute("music.changed_count", static_cast<int64_t>(result.diff.changed.size()));
        span->setAttribute("music.kept_count", static_cast<int64_t>(result.diff.kept.size()));
        span->setSuccess();
    }
    return RefineResult{std::move(result)};
}

Result<api::MusicPlanResult> MusicService::plan(const api::MusicPlanRequest &request,
                                                std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = requestChildSpan("MusicService.plan", parentSpan);
    using PlanResult = Result<api::MusicPlanResult>;
    const auto fail = [&span](ServerError error, const std::string &type) {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return PlanResult{std::move(error)};
    };
    if (span) {
        span->setAttribute("music.model_id", request.modelId);
        span->setAttribute("music.length_ms", request.musicLengthMs);
        span->setAttribute("music.prompt_length", static_cast<int64_t>(request.prompt.size()));
        span->setAttribute("music.source_plan_present", request.sourceSections.has_value());
    }
    if (!creatures::config) {
        return fail(ServerError(ServerError::InternalError, "music dependencies are unavailable"),
                    "DependencyUnavailable");
    }
    voice::MusicClient client;
    nlohmann::json source = nullptr;
    if (request.sourceSections) {
        source = {{"chunks", voice::musicSectionsToJson(*request.sourceSections)}};
    }
    auto planned = client.generatePlan(creatures::config->getVoiceApiKey(), request.prompt, request.musicLengthMs,
                                       request.modelId, source, span);
    if (!planned.isSuccess()) {
        return fail(planned.getError().value(), "ElevenLabsPlanError");
    }
    auto sections = sectionsFromPlan(planned.getValue()->compositionPlan);
    if (!sections.isSuccess()) {
        return fail(sections.getError().value(), "PlanNormalisationError");
    }
    api::MusicPlanResult result{request.modelId, voice::musicSectionsTotalMs(sections.getValue().value()),
                                sections.getValue().value(), planned.getValue()->compositionPlan};
    if (span) {
        span->setAttribute("music.section_count", static_cast<int64_t>(result.sections.size()));
        span->setSuccess();
    }
    return PlanResult{std::move(result)};
}

Result<std::vector<MusicPiece>> MusicService::list(std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = requestChildSpan("MusicService.list", parentSpan);
    using ListResult = Result<std::vector<MusicPiece>>;
    if (!creatures::db) {
        recordSpanError(span, "music database is unavailable", "DependencyUnavailable", ServerError::InternalError);
        return ListResult{ServerError(ServerError::InternalError, "music database is unavailable")};
    }
    auto pieces = creatures::db->listMusicPieces(span);
    if (!pieces.isSuccess()) {
        recordSpanError(span, pieces.getError().value().getMessage(), "MusicPieceListError",
                        pieces.getError().value().getCode());
        return pieces;
    }
    if (span) {
        span->setAttribute("music.piece_count", static_cast<int64_t>(pieces.getValue().value().size()));
        span->setSuccess();
    }
    return pieces;
}

Result<MusicPiece> MusicService::get(const std::string &pieceId, std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = requestChildSpan("MusicService.get", parentSpan);
    if (span)
        span->setAttribute("music.piece_id", pieceId);
    if (!creatures::db) {
        recordSpanError(span, "music database is unavailable", "DependencyUnavailable", ServerError::InternalError);
        return Result<MusicPiece>{ServerError(ServerError::InternalError, "music database is unavailable")};
    }
    auto piece = creatures::db->getMusicPiece(pieceId, span);
    if (!piece.isSuccess()) {
        recordSpanError(span, piece.getError().value().getMessage(), "MusicPieceLookupError",
                        piece.getError().value().getCode());
        return piece;
    }
    if (span)
        span->setSuccess();
    return piece;
}

Result<MusicPiece> MusicService::update(const std::string &pieceId, const api::MusicPieceUpdateRequest &request,
                                        std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = requestChildSpan("MusicService.update", parentSpan);
    using UpdateResult = Result<MusicPiece>;
    const auto fail = [&span](ServerError error, const std::string &type) {
        recordSpanError(span, error.getMessage(), type, error.getCode());
        return UpdateResult{std::move(error)};
    };
    if (span)
        span->setAttribute("music.piece_id", pieceId);
    if (!creatures::db) {
        return fail(ServerError(ServerError::InternalError, "music database is unavailable"), "DependencyUnavailable");
    }
    auto loaded = creatures::db->getMusicPiece(pieceId, span);
    if (!loaded.isSuccess()) {
        return fail(loaded.getError().value(), "MusicPieceLookupError");
    }
    auto piece = loaded.getValue().value();
    if (request.title)
        piece.title = *request.title;
    if (request.notes)
        piece.notes = *request.notes;
    if (request.currentVersionId) {
        if (!piece.findVersion(*request.currentVersionId)) {
            return fail(ServerError(ServerError::InvalidData,
                                    fmt::format("piece {} has no version {}", pieceId, *request.currentVersionId)),
                        "MusicVersionNotFound");
        }
        piece.current_version_id = *request.currentVersionId;
    }
    piece.updated_at = std::max(nowMillis(), piece.updated_at + 1);
    auto stored = storage::publishMusicPiece(piece, span);
    if (!stored.isSuccess()) {
        return fail(stored.getError().value(), "MusicPiecePublishError");
    }
    if (span)
        span->setSuccess();
    return stored;
}

Result<void> MusicService::remove(const std::string &pieceId, std::shared_ptr<RequestSpan> parentSpan) const {
    auto span = requestChildSpan("MusicService.remove", parentSpan);
    if (span)
        span->setAttribute("music.piece_id", pieceId);
    if (!creatures::db) {
        recordSpanError(span, "music database is unavailable", "DependencyUnavailable", ServerError::InternalError);
        return Result<void>{ServerError(ServerError::InternalError, "music database is unavailable")};
    }
    // WAVs are retained, as dialog promotion's are: the library record goes,
    // the audio does not become unrecoverable.
    auto deleted = storage::deleteMusicPiece(pieceId, span);
    if (!deleted.isSuccess()) {
        recordSpanError(span, deleted.getError().value().getMessage(), "MusicPieceDeleteError",
                        deleted.getError().value().getCode());
        return deleted;
    }
    if (span)
        span->setSuccess();
    return deleted;
}

} // namespace creatures::ws
