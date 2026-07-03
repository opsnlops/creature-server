#pragma once

#include <chrono>
#include <cstring>
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

#include "server/audio/OggOpusWriter.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/voice/DialogCache.h"
#include "server/voice/DialogClient.h"
#include "server/voice/DialogPipeline.h"
#include "server/voice/DialogWav.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
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
class DialogPreviewController : public oatpp::web::server::api::ApiController,
                                public HttpResponseHelpers<DialogPreviewController> {
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
    static void populateMetaResponse(oatpp::Object<DialogPreviewMetaResponseDto> &dto,
                                     const creatures::voice::CachedGeneration &gen, const std::string &cacheKey,
                                     bool cached) {
        dto->cache_key = cacheKey.c_str();
        dto->generation_id = gen.generationId.c_str();
        dto->cached = cached;
        dto->audio_url =
            fmt::format("/api/v1/animation/dialog/preview/audio/{}/{}.wav", cacheKey, gen.generationId).c_str();
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

  private:
    /// Shared validation for the two POST preview endpoints (/meta and
    /// /multichannel). Returns nullptr on success; an error response otherwise.
    template <typename SpanT>
    std::shared_ptr<OutgoingResponse> validatePreviewBody(const oatpp::Object<DialogPreviewRequestDto> &body,
                                                          const SpanT &span) {
        if (!body || !body->turns || body->turns->empty()) {
            return bailHttp(span, Status::CODE_400, "turns must be a non-empty array");
        }
        for (const auto &t : *body->turns) {
            if (!t || !t->creature_id || t->creature_id->empty() || !t->text || t->text->empty()) {
                return bailHttp(span, Status::CODE_400, "every turn must have a non-empty creature_id and text");
            }
        }
        return nullptr;
    }

    /// Holds everything an endpoint needs after the shared "resolve creatures
    /// → find or create a generation" pipeline has run.
    struct LoadOrGenerateOutcome {
        // Either set: an HTTP response to return immediately (validation/error path).
        std::shared_ptr<OutgoingResponse> errorResponse;
        // Or set (on success):
        std::optional<creatures::voice::CachedGeneration> generation;
        std::string cacheKey;
        std::vector<creatures::voice::DialogInput> inputs;
        std::unordered_map<std::string, PreviewCreature> resolved;
        bool cached = false;
    };

    /// Shared "resolve creatures → check cache → maybe call ElevenLabs" logic.
    /// Used by both POST /meta and POST /multichannel — both need the cached
    /// generation, they just package it differently. On failure, sets
    /// `outcome.errorResponse` and leaves `generation` empty; caller short-
    /// circuits with that response.
    template <typename SpanT>
    LoadOrGenerateOutcome loadOrGenerate(const oatpp::Object<DialogPreviewRequestDto> &body, const SpanT &span,
                                         const std::shared_ptr<creatures::OperationSpan> &opSpan,
                                         const char *spanAttrName) {
        LoadOrGenerateOutcome out;

        auto resolvedResult = resolveCreatures(body->turns, opSpan);
        if (!resolvedResult.isSuccess()) {
            // resolveCreatures returns InvalidData on bad input (missing voice.voice_id, etc.)
            // — bailFromServerError maps that to 400.
            out.errorResponse = bailFromServerError(span, resolvedResult.getError().value());
            return out;
        }
        out.resolved = resolvedResult.getValue().value();
        out.inputs = buildDialogInputs(body->turns, out.resolved);
        out.cacheKey = creatures::voice::computeCacheKey(out.inputs);

        if (span) {
            span->setAttribute("dialog.cache_key", out.cacheKey);
            span->setAttribute("dialog.turns", static_cast<int64_t>(out.inputs.size()));
            span->setAttribute("dialog.endpoint", spanAttrName);
        }

        const bool regen = body->regenerate ? static_cast<bool>(*body->regenerate) : false;

        if (body->generation_id && !body->generation_id->empty()) {
            // Explicit generation_id — load that one specifically. 404 if
            // it's been cron-cleaned.
            auto loadResult = creatures::voice::loadGeneration(out.cacheKey, std::string(*body->generation_id));
            if (!loadResult.isSuccess()) {
                out.errorResponse = bailHttp(span, Status::CODE_404,
                                             fmt::format("generation '{}' not found (expired or never existed)",
                                                         std::string(*body->generation_id)));
                return out;
            }
            out.generation = loadResult.getValue().value();
            out.cached = true;
        } else if (!regen) {
            if (auto latest = creatures::voice::findLatestGeneration(out.cacheKey)) {
                auto loadResult = creatures::voice::loadGeneration(out.cacheKey, *latest);
                if (loadResult.isSuccess()) {
                    out.generation = loadResult.getValue().value();
                    out.cached = true;
                }
                // If the load failed for some odd reason, fall through to
                // fresh generation — the cache lookup is advisory.
            }
        }

        if (!out.generation) {
            // No usable cache hit (or regenerate requested) — fresh ElevenLabs.
            creatures::voice::DialogClient client;
            const std::string apiKey = creatures::config->getVoiceApiKey();

            auto dialogResult = client.generateDialog(apiKey, out.inputs, "pcm_48000", opSpan);
            if (!dialogResult.isSuccess()) {
                out.errorResponse = bailFromServerError(span, dialogResult.getError().value());
                return out;
            }
            const auto dialog = dialogResult.getValue().value();

            std::string transcript;
            for (std::size_t t = 0; t < out.inputs.size(); ++t) {
                if (t > 0) {
                    transcript.push_back(' ');
                }
                transcript += creatures::voice::DialogClient::stripTags(out.inputs[t].text);
            }
            const auto wavBytes = wrapMonoPcmAsWav(dialog.audioData, 48000);
            auto alignResult = client.forcedAlignment(apiKey, wavBytes, "audio/wav", transcript, opSpan);
            if (!alignResult.isSuccess()) {
                out.errorResponse = bailFromServerError(span, alignResult.getError().value());
                return out;
            }
            creatures::voice::CachedGeneration freshGen;
            freshGen.generationId = util::generateUUID();
            freshGen.audioPcm = dialog.audioData;
            freshGen.voiceSegments = dialog.voiceSegments;
            freshGen.forcedAlignment = alignResult.getValue().value();
            freshGen.createdAt = std::chrono::system_clock::now();
            freshGen.turnsSummary = makeTurnsSummary(out.inputs);

            auto saveResult = creatures::voice::saveGeneration(out.cacheKey, freshGen);
            if (!saveResult.isSuccess()) {
                // Generation succeeded but couldn't be persisted — log and
                // continue; the client still gets the audio.
                warn("Dialog preview: saveGeneration failed: {}", saveResult.getError().value().getMessage());
            }

            out.generation = std::move(freshGen);
            out.cached = false;
        }

        if (span && out.generation) {
            span->setAttribute("dialog.generation_id", out.generation->generationId);
            span->setAttribute("dialog.cached", out.cached);
        }
        return out;
    }

  public:
    ENDPOINT_INFO(submitPreviewMeta) {
        info->summary = "Generate (or load) a dialog preview and return its metadata + audio URL";
        info->description =
            "Returns small JSON: cache_key, generation_id, cached flag, audio_url (GET this for the mono WAV "
            "ready for an <audio> element), audio_format, sample_rate, duration_seconds, voice_segments, and "
            "forced-alignment word/char timings. Generates fresh via ElevenLabs if no cache hit (or "
            "regenerate=true); reuses the latest cached take by default; loads a specific take if generation_id "
            "is set (404 if expired).";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<DialogPreviewMetaResponseDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/meta", submitPreviewMeta,
             BODY_DTO(Object<DialogPreviewRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/animation/dialog/preview/meta", "POST", "api/v1/animation/dialog/preview/meta",
                           "submitPreviewMeta", "DialogPreviewController", request,
                           [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               if (auto errResp = validatePreviewBody(requestBody, span))
                                   return errResp;
                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "DialogPreviewController.submitPreviewMeta", span);
                               auto outcome = loadOrGenerate(requestBody, span, opSpan, "meta");
                               if (outcome.errorResponse)
                                   return outcome.errorResponse;

                               auto dto = DialogPreviewMetaResponseDto::createShared();
                               populateMetaResponse(dto, *outcome.generation, outcome.cacheKey, outcome.cached);
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, dto);
                           });
    }

    ENDPOINT_INFO(getPreviewAudio) {
        info->summary = "Stream the mono WAV for a cached dialog generation";
        info->description = "Reads the cached mono PCM for {cache_key}/{generation_id}, wraps it in a 44-byte "
                            "PCM WAV header on the fly, and streams audio/wav back. URL is built from the "
                            "audio_url field of a /preview/meta response. 404 if the generation isn't cached "
                            "(never existed or has been cron-swept).";
        info->addTag("Multi-character Dialog");
        info->pathParams["cache_key"].description = "Hex sha256 of the turns; from /preview/meta or /preview/lookup.";
        info->pathParams["generation_id"].description = "UUID of the specific take; from /preview/meta or /lookup.";
        info->addResponse<oatpp::String>(Status::CODE_200, "audio/wav");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    // oatpp's URL Pattern matcher only checks parts at `/` boundaries — a
    // literal suffix after a path variable (`{var}.wav`) is parsed but never
    // matches. So we make the whole last segment one variable (`{filename}`)
    // and strip the `.wav` server-side.
    ENDPOINT("GET", "api/v1/animation/dialog/preview/audio/{cache_key}/{filename}", getPreviewAudio,
             PATH(String, cache_key), PATH(String, filename), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/dialog/preview/audio/{cache_key}/{filename}", "GET",
            "api/v1/animation/dialog/preview/audio/{cache_key}/{filename}", "getPreviewAudio",
            "DialogPreviewController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const std::string ck = cache_key ? std::string(*cache_key) : std::string();
                std::string gid = filename ? std::string(*filename) : std::string();
                // Accept either {id} or {id}.wav (preferred for
                // browser save-as / Content-Type sniffing).
                if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".wav") == 0) {
                    gid.resize(gid.size() - 4);
                }
                if (ck.empty() || gid.empty()) {
                    return bailHttp(span, Status::CODE_400, "cache_key and generation_id are required");
                }
                auto loadResult = creatures::voice::loadGeneration(ck, gid);
                if (!loadResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_404, fmt::format("generation '{}/{}' not found", ck, gid));
                }
                const auto gen = loadResult.getValue().value();
                const auto wavBytes = wrapMonoPcmAsWav(gen.audioPcm, 48000);

                auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(wavBytes.data()),
                                                    static_cast<v_int32>(wavBytes.size())));
                response->putHeader("Content-Type", "audio/wav");
                response->putHeader("X-Dialog-Cache-Key", ck.c_str());
                response->putHeader("X-Dialog-Generation-Id", gid.c_str());
                if (span) {
                    span->setAttribute("dialog.cache_key", ck);
                    span->setAttribute("dialog.generation_id", gid);
                    span->setHttpStatus(200);
                }
                return response;
            });
    }

    ENDPOINT_INFO(getPreviewShareable) {
        info->summary = "Download a shareable Ogg/Opus version of a cached dialog generation";
        info->description = "Encodes the cached mono PCM for {cache_key}/{generation_id} to Ogg/Opus (96 kbps, "
                            "mono) for sharing. URL is built from the cache_key/generation_id of a /preview/meta "
                            "response. 404 if the generation isn't cached (never existed or has been cron-swept).";
        info->addTag("Multi-character Dialog");
        info->pathParams["cache_key"].description = "Hex sha256 of the turns; from /preview/meta or /preview/lookup.";
        info->pathParams["generation_id"].description = "UUID of the specific take; from /preview/meta or /lookup.";
        info->addResponse<oatpp::String>(Status::CODE_200, "audio/ogg");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    // Same URL-matcher caveat as getPreviewAudio: the whole last segment is one
    // variable and the extension is stripped server-side.
    ENDPOINT("GET", "api/v1/animation/dialog/preview/share/{cache_key}/{filename}", getPreviewShareable,
             PATH(String, cache_key), PATH(String, filename), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/dialog/preview/share/{cache_key}/{filename}", "GET",
            "api/v1/animation/dialog/preview/share/{cache_key}/{filename}", "getPreviewShareable",
            "DialogPreviewController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const std::string ck = cache_key ? std::string(*cache_key) : std::string();
                std::string gid = filename ? std::string(*filename) : std::string();
                if (gid.size() > 4 && gid.compare(gid.size() - 4, 4, ".ogg") == 0) {
                    gid.resize(gid.size() - 4);
                }
                if (ck.empty() || gid.empty()) {
                    return bailHttp(span, Status::CODE_400, "cache_key and generation_id are required");
                }
                auto loadResult = creatures::voice::loadGeneration(ck, gid);
                if (!loadResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_404, fmt::format("generation '{}/{}' not found", ck, gid));
                }
                const auto gen = loadResult.getValue().value();

                // The cache stores raw S16LE bytes; the encoder wants samples.
                std::vector<int16_t> samples(gen.audioPcm.size() / sizeof(int16_t));
                std::memcpy(samples.data(), gen.audioPcm.data(), samples.size() * sizeof(int16_t));

                auto oggResult = creatures::audio::encodeMonoToOggOpus(samples, creatures::audio::kShareableSampleRate);
                if (!oggResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_500, oggResult.getError().value().getMessage());
                }
                const auto oggBytes = oggResult.getValue().value();
                const auto shareName = fmt::format("dialog-preview-{}.ogg", gid.substr(0, 8));

                auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(reinterpret_cast<const char *>(oggBytes.data()),
                                                    static_cast<v_int32>(oggBytes.size())));
                response->putHeader("Content-Type", "audio/ogg");
                response->putHeader("Content-Disposition", "attachment; filename=\"" + shareName + "\"");
                response->putHeader("X-Dialog-Cache-Key", ck.c_str());
                response->putHeader("X-Dialog-Generation-Id", gid.c_str());
                if (span) {
                    span->setAttribute("dialog.cache_key", ck);
                    span->setAttribute("dialog.generation_id", gid);
                    span->setAttribute("share.bytes", static_cast<int64_t>(oggBytes.size()));
                    span->setHttpStatus(200);
                }
                return response;
            });
    }

    ENDPOINT_INFO(submitPreviewMultichannel) {
        info->summary = "Generate (or load) a dialog preview and return the assembled 17-channel WAV";
        info->description = "Same cache semantics as /preview/meta (use generation_id / regenerate flags the same "
                            "way). Returns audio/wav bytes — the 17-channel WAV the show would play. Suitable for "
                            "downloading into Audacity (or any 17-channel-aware tool) for inspection. Each "
                            "creature's audio appears in its `audio_channel` lane; all other lanes are silent.";
        info->addTag("Multi-character Dialog");
        info->addResponse<oatpp::String>(Status::CODE_200, "audio/wav");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/preview/multichannel", submitPreviewMultichannel,
             BODY_DTO(Object<DialogPreviewRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/preview/multichannel", "POST",
            "api/v1/animation/dialog/preview/multichannel", "submitPreviewMultichannel", "DialogPreviewController",
            request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (auto errResp = validatePreviewBody(requestBody, span))
                    return errResp;
                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogPreviewController.submitPreviewMultichannel", span);
                auto outcome = loadOrGenerate(requestBody, span, opSpan, "multichannel");
                if (outcome.errorResponse)
                    return outcome.errorResponse;

                // Validate the audio_channel mapping (distinct, in [1,17]).
                // 400 instead of 500 since this is a caller config problem.
                std::unordered_set<uint16_t> seenChannels;
                creatures::voice::VoiceChannelMap voiceToChannel;
                for (const auto &[cid, c] : outcome.resolved) {
                    if (c.audioChannel < 1 || c.audioChannel > 17) {
                        return bailHttp(
                            span, Status::CODE_400,
                            fmt::format("creature '{}' has audio_channel {} (must be 1..17)", cid, c.audioChannel));
                    }
                    if (!seenChannels.insert(c.audioChannel).second) {
                        return bailHttp(
                            span, Status::CODE_400,
                            fmt::format("audio_channel {} is assigned to more than one creature in this scene",
                                        c.audioChannel));
                    }
                    voiceToChannel.emplace(c.voiceId, c.audioChannel);
                }

                creatures::voice::DialogResult dr;
                dr.audioData = outcome.generation->audioPcm;
                dr.audioFormat = "pcm_48000";
                dr.voiceSegments = outcome.generation->voiceSegments;
                auto assembleResult =
                    creatures::voice::assembleChunk(outcome.inputs, dr, outcome.generation->forcedAlignment, 48000);
                if (!assembleResult.isSuccess()) {
                    return bailFromServerError(span, assembleResult.getError().value());
                }
                const auto assembled = assembleResult.getValue().value();

                // One-shot temp file under the dialog-cache dir so the cron
                // sweep cleans it up. We don't bother caching the multichannel
                // WAV — cheap to rebuild from the cached generation, and would
                // need a second cache key keyed on the creature→channel map.
                const auto wavPath = std::filesystem::temp_directory_path() / "creature-adhoc" / "dialog-cache" /
                                     outcome.cacheKey /
                                     fmt::format("{}_multichannel.wav", outcome.generation->generationId);
                std::error_code ec;
                std::filesystem::create_directories(wavPath.parent_path(), ec);
                auto writeResult = creatures::voice::writeDialogWav(assembled, voiceToChannel, wavPath, opSpan);
                if (!writeResult.isSuccess()) {
                    return bailFromServerError(span, writeResult.getError().value());
                }
                std::ifstream in(wavPath, std::ios::binary);
                std::vector<char> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
                    Status::CODE_200, oatpp::String(buf.data(), static_cast<v_int32>(buf.size())));
                response->putHeader("Content-Type", "audio/wav");
                response->putHeader("X-Dialog-Generation-Id", outcome.generation->generationId.c_str());
                response->putHeader("X-Dialog-Cache-Key", outcome.cacheKey.c_str());
                response->putHeader("X-Dialog-Cached", outcome.cached ? "true" : "false");
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
                    return bailHttp(span, Status::CODE_400, "turns must be a non-empty array");
                }
                for (const auto &t : *requestBody->turns) {
                    if (!t || !t->creature_id || t->creature_id->empty() || !t->text || t->text->empty()) {
                        return bailHttp(span, Status::CODE_400,
                                        "every turn must have a non-empty creature_id and text");
                    }
                }

                auto opSpan =
                    creatures::observability->createChildOperationSpan("DialogPreviewController.lookupPreview", span);
                auto resolvedResult = resolveCreatures(requestBody->turns, opSpan);
                if (!resolvedResult.isSuccess()) {
                    return bailFromServerError(span, resolvedResult.getError().value());
                }
                const auto inputs = buildDialogInputs(requestBody->turns, resolvedResult.getValue().value());
                const auto cacheKey = creatures::voice::computeCacheKey(inputs);
                const auto generations = creatures::voice::listGenerations(cacheKey);

                if (generations.empty()) {
                    if (span) {
                        span->setAttribute("dialog.cache_key", cacheKey);
                    }
                    return bailHttp(span, Status::CODE_404, "no cached generations for these turns");
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
