#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <base64.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/voice/DialogCache.h"
#include "server/voice/DialogClient.h"
#include "server/voice/DialogPipeline.h"
#include "server/voice/DialogWav.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/dto/DialogDto.h"
#include "server/ws/dto/StatusDto.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures::ws {

/// HTTP surface for the dialog preview / cache (companion to DialogController).
///
/// Two endpoints:
///   * POST /api/v1/animation/dialog/preview        — generate/return a take
///   * POST /api/v1/animation/dialog/preview/lookup — check what's cached
///
/// The preview path returns either JSON (mono format, default) or audio/wav
/// bytes (multichannel format, for downloading + inspecting in Audacity).
/// Both share the same cache lookup; the multichannel branch additionally
/// runs the per-creature slice + 17-channel WAV assembly.
class DialogPreviewController : public oatpp::web::server::api::ApiController {
  public:
    DialogPreviewController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : ApiController(objectMapper) {}

    static std::shared_ptr<DialogPreviewController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                                 objectMapper)) {
        return std::make_shared<DialogPreviewController>(objectMapper);
    }

  private:
    /// Per-creature info the preview needs from the DB. Mono-format previews
    /// only need voiceId; multichannel additionally needs audioChannel.
    struct PreviewCreature {
        std::string creatureId;
        std::string voiceId;
        uint16_t audioChannel; // 1-based; used only for multichannel format
    };

    /// Walk the request's turns, resolve each unique creature_id to its
    /// voice_id (+ audio_channel for multichannel use). Caller maps the
    /// Result error to an HTTP status.
    static creatures::Result<std::unordered_map<std::string, PreviewCreature>>
    resolveCreatures(const oatpp::List<oatpp::Object<DialogTurnDto>> &turns,
                     const std::shared_ptr<creatures::OperationSpan> &span) {
        std::unordered_map<std::string, PreviewCreature> resolved;
        for (const auto &t : *turns) {
            const std::string cid(*t->creature_id);
            if (resolved.count(cid)) {
                continue;
            }
            auto jr = creatures::db->getCreatureJson(cid, span);
            if (!jr.isSuccess()) {
                return creatures::Result<std::unordered_map<std::string, PreviewCreature>>{creatures::ServerError(
                    creatures::ServerError::InvalidData,
                    fmt::format("creature '{}' lookup failed: {}", cid, jr.getError().value().getMessage()))};
            }
            const auto cj = jr.getValue().value();
            if (!cj.contains("voice") || !cj["voice"].is_object() || !cj["voice"].contains("voice_id") ||
                !cj["voice"]["voice_id"].is_string()) {
                return creatures::Result<std::unordered_map<std::string, PreviewCreature>>{creatures::ServerError(
                    creatures::ServerError::InvalidData, fmt::format("creature '{}' has no voice.voice_id", cid))};
            }
            PreviewCreature pc;
            pc.creatureId = cid;
            pc.voiceId = cj["voice"]["voice_id"].get<std::string>();
            pc.audioChannel = cj.contains("audio_channel") && cj["audio_channel"].is_number()
                                  ? cj["audio_channel"].get<uint16_t>()
                                  : 0;
            resolved.emplace(cid, std::move(pc));
        }
        return resolved;
    }

    /// Convert the API DTO turns + resolved creature lookup into the
    /// internal DialogInput list (voice_id + text).
    static std::vector<creatures::voice::DialogInput>
    buildDialogInputs(const oatpp::List<oatpp::Object<DialogTurnDto>> &turns,
                      const std::unordered_map<std::string, PreviewCreature> &resolved) {
        std::vector<creatures::voice::DialogInput> out;
        out.reserve(turns->size());
        for (const auto &t : *turns) {
            const std::string cid(*t->creature_id);
            const std::string text(*t->text);
            const auto &c = resolved.at(cid);
            out.push_back({c.voiceId, text});
        }
        return out;
    }

    /// Build a short summary for the cache metadata (first ~80 chars of joined
    /// turn text). Just for human-readable debugging when ls'ing the cache dir.
    static std::string makeTurnsSummary(const std::vector<creatures::voice::DialogInput> &inputs) {
        std::string s;
        for (const auto &i : inputs) {
            if (!s.empty()) {
                s.push_back(' ');
            }
            s += creatures::voice::DialogClient::stripTags(i.text);
            if (s.size() >= 80) {
                s.resize(80);
                s += "…";
                break;
            }
        }
        return s;
    }

    /// Pack our internal voice_segments / forced_alignment into the response DTO.
    static void populateMonoResponse(oatpp::Object<DialogPreviewMonoResponseDto> &dto,
                                     const creatures::voice::CachedGeneration &gen, const std::string &cacheKey,
                                     bool cached) {
        dto->cache_key = cacheKey.c_str();
        dto->generation_id = gen.generationId.c_str();
        dto->cached = cached;
        dto->audio_base64 =
            base64::to_base64(std::string(reinterpret_cast<const char *>(gen.audioPcm.data()), gen.audioPcm.size()))
                .c_str();
        dto->audio_format = "pcm_48000";
        dto->sample_rate = 48000;
        dto->duration_seconds = static_cast<double>(gen.audioPcm.size()) / (48000.0 * 2.0); // mono S16

        auto segs = oatpp::List<oatpp::Object<DialogPreviewVoiceSegmentDto>>::createShared();
        for (const auto &s : gen.voiceSegments) {
            auto sd = DialogPreviewVoiceSegmentDto::createShared();
            sd->voice_id = s.voiceId.c_str();
            sd->character_start_index = static_cast<v_uint64>(s.characterStartIndex);
            sd->character_end_index = static_cast<v_uint64>(s.characterEndIndex);
            sd->dialog_input_index = static_cast<v_uint64>(s.dialogInputIndex);
            segs->push_back(sd);
        }
        dto->voice_segments = segs;

        auto words = oatpp::List<oatpp::Object<DialogPreviewWordTimingDto>>::createShared();
        for (const auto &w : gen.forcedAlignment.words) {
            auto wd = DialogPreviewWordTimingDto::createShared();
            wd->text = w.text.c_str();
            wd->start = w.startSeconds;
            wd->end = w.endSeconds;
            words->push_back(wd);
        }
        dto->forced_alignment_words = words;

        auto chars = oatpp::List<oatpp::Object<DialogPreviewCharTimingDto>>::createShared();
        for (const auto &c : gen.forcedAlignment.characters) {
            auto cd = DialogPreviewCharTimingDto::createShared();
            cd->text = c.text.c_str();
            cd->start = c.startSeconds;
            cd->end = c.endSeconds;
            chars->push_back(cd);
        }
        dto->forced_alignment_chars = chars;

        dto->forced_alignment_loss = gen.forcedAlignment.loss;
    }

    /// Wrap raw mono S16LE PCM in a canonical 44-byte PCM WAV header.
    /// Duplicate of JobWorker.cpp's helper; will be deduped when the
    /// storage facade lands (issue #11).
    static std::vector<uint8_t> wrapMonoPcmAsWav(const std::vector<uint8_t> &pcm, uint32_t sampleRate) {
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
        u32(16);
        u16(1); // PCM
        u16(1); // mono
        u32(sampleRate);
        u32(sampleRate * 2); // byte rate (mono S16)
        u16(2);              // block align
        u16(16);             // bits per sample
        str("data", 4);
        u32(dataLen);
        out.insert(out.end(), pcm.begin(), pcm.end());
        return out;
    }

  public:
    ENDPOINT_INFO(submitPreview) {
        info->summary = "Generate (or return a cached take of) a multi-character dialog preview";
        info->description =
            "Quick playback of the dialog audio without committing to a full animation. Two output formats: "
            "'mono' (default) returns JSON with the raw single-channel PCM + voice_segments + forced-alignment "
            "timing, suitable for UI playback + previewing mouth timing. 'multichannel' returns the actual "
            "17-channel WAV the show would play, as audio/wav bytes — for downloading into Audacity for "
            "inspection. Cached on disk by sha256(turns); pass `generation_id` to re-fetch a specific take, "
            "`regenerate: true` to force a fresh take.";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<DialogPreviewMonoResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_200, "audio/wav");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview", submitPreview,
             BODY_DTO(Object<DialogPreviewRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview", "POST", "api/v1/animation/dialog/preview", "submitPreview",
            "DialogPreviewController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!requestBody || !requestBody->turns || requestBody->turns->empty()) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 400;
                    err->message = "turns must be a non-empty array";
                    if (span)
                        span->setHttpStatus(400);
                    return createDtoResponse(Status::CODE_400, err);
                }
                for (const auto &t : *requestBody->turns) {
                    if (!t || !t->creature_id || t->creature_id->empty() || !t->text || t->text->empty()) {
                        auto err = StatusDto::createShared();
                        err->status = "error";
                        err->code = 400;
                        err->message = "every turn must have a non-empty creature_id and text";
                        if (span)
                            span->setHttpStatus(400);
                        return createDtoResponse(Status::CODE_400, err);
                    }
                }
                const std::string format =
                    requestBody->format ? std::string(*requestBody->format) : std::string("mono");
                if (format != "mono" && format != "multichannel") {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 400;
                    err->message = "format must be 'mono' or 'multichannel'";
                    if (span)
                        span->setHttpStatus(400);
                    return createDtoResponse(Status::CODE_400, err);
                }

                // Wrap the request span as a child OperationSpan so the DB +
                // voice helpers (which want OperationSpan, not RequestSpan)
                // are still traced under this request.
                auto opSpan =
                    creatures::observability->createChildOperationSpan("DialogPreviewController.submitPreview", span);
                auto resolvedResult = resolveCreatures(requestBody->turns, opSpan);
                if (!resolvedResult.isSuccess()) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 400;
                    err->message = resolvedResult.getError().value().getMessage().c_str();
                    if (span)
                        span->setHttpStatus(400);
                    return createDtoResponse(Status::CODE_400, err);
                }
                const auto resolved = resolvedResult.getValue().value();
                const auto inputs = buildDialogInputs(requestBody->turns, resolved);
                const auto cacheKey = creatures::voice::computeCacheKey(inputs);

                if (span) {
                    span->setAttribute("dialog.cache_key", cacheKey);
                    span->setAttribute("dialog.turns", static_cast<int64_t>(inputs.size()));
                    span->setAttribute("dialog.format", format);
                }

                // ---- Resolve which cached generation to use, or generate fresh.
                std::optional<creatures::voice::CachedGeneration> gen;
                bool wasCached = false;
                const bool regen = requestBody->regenerate ? static_cast<bool>(*requestBody->regenerate) : false;

                if (requestBody->generation_id && !requestBody->generation_id->empty()) {
                    // Explicit generation_id — load that one specifically. 404
                    // if it's been cron-cleaned.
                    auto loadResult =
                        creatures::voice::loadGeneration(cacheKey, std::string(*requestBody->generation_id));
                    if (!loadResult.isSuccess()) {
                        auto err = StatusDto::createShared();
                        err->status = "error";
                        err->code = 404;
                        err->message = fmt::format("generation '{}' not found (expired or never existed)",
                                                   std::string(*requestBody->generation_id))
                                           .c_str();
                        if (span)
                            span->setHttpStatus(404);
                        return createDtoResponse(Status::CODE_404, err);
                    }
                    gen = loadResult.getValue().value();
                    wasCached = true;
                } else if (!regen) {
                    if (auto latest = creatures::voice::findLatestGeneration(cacheKey)) {
                        auto loadResult = creatures::voice::loadGeneration(cacheKey, *latest);
                        if (loadResult.isSuccess()) {
                            gen = loadResult.getValue().value();
                            wasCached = true;
                        }
                        // If the load failed for some odd reason, fall through
                        // to fresh generation — the cache lookup is advisory.
                    }
                }

                if (!gen) {
                    // No usable cache hit (or regenerate requested) — make a
                    // fresh generation via ElevenLabs.
                    creatures::voice::DialogClient client;
                    const std::string apiKey = creatures::config->getVoiceApiKey();

                    auto dialogResult = client.generateDialog(apiKey, inputs, "pcm_48000", opSpan);
                    if (!dialogResult.isSuccess()) {
                        auto code = dialogResult.getError().value().getCode();
                        auto err = StatusDto::createShared();
                        err->status = "error";
                        err->code = (code == creatures::ServerError::InvalidData) ? 400 : 500;
                        err->message = dialogResult.getError().value().getMessage().c_str();
                        if (span)
                            span->setHttpStatus(*err->code);
                        return createDtoResponse(
                            (code == creatures::ServerError::InvalidData) ? Status::CODE_400 : Status::CODE_500, err);
                    }
                    const auto dialog = dialogResult.getValue().value();

                    std::string transcript;
                    for (std::size_t t = 0; t < inputs.size(); ++t) {
                        if (t > 0) {
                            transcript.push_back(' ');
                        }
                        transcript += creatures::voice::DialogClient::stripTags(inputs[t].text);
                    }
                    // forced-alignment wants a WAV upload; wrap the raw PCM.
                    const auto wavBytes = wrapMonoPcmAsWav(dialog.audioData, 48000);
                    auto alignResult = client.forcedAlignment(apiKey, wavBytes, "audio/wav", transcript, opSpan);
                    if (!alignResult.isSuccess()) {
                        auto err = StatusDto::createShared();
                        err->status = "error";
                        err->code = 500;
                        err->message = alignResult.getError().value().getMessage().c_str();
                        if (span)
                            span->setHttpStatus(500);
                        return createDtoResponse(Status::CODE_500, err);
                    }
                    creatures::voice::CachedGeneration freshGen;
                    freshGen.generationId = util::generateUUID();
                    freshGen.audioPcm = dialog.audioData;
                    freshGen.voiceSegments = dialog.voiceSegments;
                    freshGen.forcedAlignment = alignResult.getValue().value();
                    freshGen.createdAt = std::chrono::system_clock::now();
                    freshGen.turnsSummary = makeTurnsSummary(inputs);

                    auto saveResult = creatures::voice::saveGeneration(cacheKey, freshGen);
                    if (!saveResult.isSuccess()) {
                        // Generation succeeded but couldn't be persisted — log
                        // and continue; the client still gets the audio. (The
                        // tradeoff: same input later won't be free, but the
                        // current response is correct.)
                        warn("Dialog preview: saveGeneration failed: {}", saveResult.getError().value().getMessage());
                    }

                    gen = std::move(freshGen);
                    wasCached = false;
                }

                if (span) {
                    span->setAttribute("dialog.generation_id", gen->generationId);
                    span->setAttribute("dialog.cached", wasCached);
                }

                // ---- Format the response.
                if (format == "mono") {
                    auto dto = DialogPreviewMonoResponseDto::createShared();
                    populateMonoResponse(dto, *gen, cacheKey, wasCached);
                    if (span)
                        span->setHttpStatus(200);
                    return createDtoResponse(Status::CODE_200, dto);
                }

                // ---- multichannel: build the 17-channel WAV from the cached
                // generation + creature config and stream it back as audio/wav.
                // Validate the audio_channel mapping (distinct, in [1,17]) the
                // same way the writer does — but fail with 400 instead of 500
                // since this is a caller config problem.
                std::unordered_set<uint16_t> seenChannels;
                creatures::voice::VoiceChannelMap voiceToChannel;
                for (const auto &[cid, c] : resolved) {
                    if (c.audioChannel < 1 || c.audioChannel > 17) {
                        auto err = StatusDto::createShared();
                        err->status = "error";
                        err->code = 400;
                        err->message =
                            fmt::format("creature '{}' has audio_channel {} (must be 1..17)", cid, c.audioChannel)
                                .c_str();
                        if (span)
                            span->setHttpStatus(400);
                        return createDtoResponse(Status::CODE_400, err);
                    }
                    if (!seenChannels.insert(c.audioChannel).second) {
                        auto err = StatusDto::createShared();
                        err->status = "error";
                        err->code = 400;
                        err->message =
                            fmt::format("audio_channel {} is assigned to more than one creature in this scene",
                                        c.audioChannel)
                                .c_str();
                        if (span)
                            span->setHttpStatus(400);
                        return createDtoResponse(Status::CODE_400, err);
                    }
                    voiceToChannel.emplace(c.voiceId, c.audioChannel);
                }

                // Run the slice + assembly on the cached PCM + alignment.
                creatures::voice::DialogResult dr;
                dr.audioData = gen->audioPcm;
                dr.audioFormat = "pcm_48000";
                dr.voiceSegments = gen->voiceSegments;
                auto assembleResult = creatures::voice::assembleChunk(inputs, dr, gen->forcedAlignment, 48000);
                if (!assembleResult.isSuccess()) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 500;
                    err->message = assembleResult.getError().value().getMessage().c_str();
                    if (span)
                        span->setHttpStatus(500);
                    return createDtoResponse(Status::CODE_500, err);
                }
                const auto assembled = assembleResult.getValue().value();

                // Write to a one-shot temp file under the same dialog-cache
                // dir so the cron sweep cleans it up. We don't bother caching
                // the multichannel WAV itself (cheap to rebuild from the
                // cached generation, and would add a second cache key keyed
                // on the creature → channel mapping).
                const auto wavPath = std::filesystem::temp_directory_path() / "creature-adhoc" / "dialog-cache" /
                                     cacheKey / fmt::format("{}_multichannel.wav", gen->generationId);
                std::error_code ec;
                std::filesystem::create_directories(wavPath.parent_path(), ec);
                auto writeResult = creatures::voice::writeDialogWav(assembled, voiceToChannel, wavPath, opSpan);
                if (!writeResult.isSuccess()) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 500;
                    err->message = writeResult.getError().value().getMessage().c_str();
                    if (span)
                        span->setHttpStatus(500);
                    return createDtoResponse(Status::CODE_500, err);
                }

                // Read the bytes back to return as the response body. (For a
                // ~few-second scene this is ~5–50 MiB; fine in memory.)
                std::ifstream in(wavPath, std::ios::binary);
                std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

                auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(buf.data(), static_cast<v_int32>(buf.size())));
                response->putHeader("Content-Type", "audio/wav");
                response->putHeader("X-Dialog-Generation-Id", gen->generationId.c_str());
                response->putHeader("X-Dialog-Cache-Key", cacheKey.c_str());
                response->putHeader("X-Dialog-Cached", wasCached ? "true" : "false");
                if (span)
                    span->setHttpStatus(200);
                return response;
            });
    }

    ENDPOINT_INFO(lookupPreview) {
        info->summary = "Check what dialog generations are cached for a given input";
        info->description =
            "Cheap cache-lookup endpoint — does no audio work. Returns the list of cached generations (newest "
            "first) for the given turns, or 404 if nothing is cached. UI can use this to badge the 'Make "
            "Animation' button as fast (cached) vs slow (will hit ElevenLabs).";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<DialogPreviewLookupResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/lookup", lookupPreview,
             BODY_DTO(Object<DialogPreviewLookupRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview/lookup", "POST", "api/v1/animation/dialog/preview/lookup",
            "lookupPreview", "DialogPreviewController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!requestBody || !requestBody->turns || requestBody->turns->empty()) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 400;
                    err->message = "turns must be a non-empty array";
                    if (span)
                        span->setHttpStatus(400);
                    return createDtoResponse(Status::CODE_400, err);
                }
                for (const auto &t : *requestBody->turns) {
                    if (!t || !t->creature_id || t->creature_id->empty() || !t->text || t->text->empty()) {
                        auto err = StatusDto::createShared();
                        err->status = "error";
                        err->code = 400;
                        err->message = "every turn must have a non-empty creature_id and text";
                        if (span)
                            span->setHttpStatus(400);
                        return createDtoResponse(Status::CODE_400, err);
                    }
                }

                auto opSpan =
                    creatures::observability->createChildOperationSpan("DialogPreviewController.lookupPreview", span);
                auto resolvedResult = resolveCreatures(requestBody->turns, opSpan);
                if (!resolvedResult.isSuccess()) {
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = 400;
                    err->message = resolvedResult.getError().value().getMessage().c_str();
                    if (span)
                        span->setHttpStatus(400);
                    return createDtoResponse(Status::CODE_400, err);
                }
                const auto inputs = buildDialogInputs(requestBody->turns, resolvedResult.getValue().value());
                const auto cacheKey = creatures::voice::computeCacheKey(inputs);
                const auto generations = creatures::voice::listGenerations(cacheKey);

                if (generations.empty()) {
                    auto err = StatusDto::createShared();
                    err->status = "not_found";
                    err->code = 404;
                    err->message = "no cached generations for these turns";
                    if (span) {
                        span->setAttribute("dialog.cache_key", cacheKey);
                        span->setHttpStatus(404);
                    }
                    return createDtoResponse(Status::CODE_404, err);
                }

                auto dto = DialogPreviewLookupResponseDto::createShared();
                dto->cache_key = cacheKey.c_str();
                dto->latest_generation_id = generations.front().generationId.c_str();
                auto entries = oatpp::List<oatpp::Object<DialogPreviewGenerationEntryDto>>::createShared();
                for (const auto &g : generations) {
                    auto ed = DialogPreviewGenerationEntryDto::createShared();
                    ed->generation_id = g.generationId.c_str();
                    // ISO-8601 from the time_point.
                    const auto secs =
                        std::chrono::duration_cast<std::chrono::seconds>(g.createdAt.time_since_epoch()).count();
                    const std::time_t tt = static_cast<std::time_t>(secs);
                    std::tm tm{};
                    gmtime_r(&tt, &tm);
                    ed->created_at = fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", tm.tm_year + 1900,
                                                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
                                         .c_str();
                    entries->push_back(ed);
                }
                dto->generations = entries;
                if (span) {
                    span->setAttribute("dialog.cache_key", cacheKey);
                    span->setAttribute("dialog.generations", static_cast<int64_t>(generations.size()));
                    span->setHttpStatus(200);
                }
                return createDtoResponse(Status::CODE_200, dto);
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
