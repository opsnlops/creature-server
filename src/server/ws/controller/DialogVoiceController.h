#pragma once

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#include <fmt/format.h>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "model/DialogScript.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/storage/Storage.h"
#include "server/voice/DialogCache.h"
#include "server/voice/ScriptCacheKey.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/DialogVoiceDto.h"
#include "server/ws/dto/StatusDto.h"
#include "util/Slugify.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures::ws {

/// Accepting a voice take (#131).
///
/// Mirrors music promotion: an explicit choice, recorded on the script, that
/// the render reads. Acceptance is a MOVE — the take's audio leaves the
/// 24h-TTL ad-hoc bucket for the permanent tree, and any take it replaces
/// moves back. The sounds directory therefore holds at most one take per
/// script.
class DialogVoiceController : public oatpp::web::server::api::ApiController,
                              public HttpResponseHelpers<DialogVoiceController> {
  public:
    DialogVoiceController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) : ApiController(objectMapper) {}

    static std::shared_ptr<DialogVoiceController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                               objectMapper)) {
        return std::make_shared<DialogVoiceController>(objectMapper);
    }

  private:
    static int64_t nowMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

  public:
    ENDPOINT_INFO(acceptVoiceTake) {
        info->summary = "Accept a voice take for a dialog script";
        info->description =
            "Records an explicit, script-level choice of voice take and promotes its audio out of the 24h-TTL "
            "ad-hoc bucket into the permanent sounds tree, so the script can reference it indefinitely.\\n\\n"
            "`dialog_cache_key` must match sha256 of the script's CURRENT turns. Accepting a take auditioned "
            "against turns that have since been edited is rejected rather than stored as immediately-stale.\\n\\n"
            "Accepting when a take is already accepted replaces it, demoting the previous audio back to ad-hoc "
            "(its TTL restarts). The sounds directory holds at most one take per script.";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<DialogScriptDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/voice/accept", acceptVoiceTake,
             BODY_DTO(Object<AcceptVoiceRequestDto>, body), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/voice/accept", "POST", "api/v1/animation/dialog/voice/accept",
            "acceptVoiceTake", "DialogVoiceController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!body) {
                    return bailHttp(span, Status::CODE_400, "Request body is required");
                }
                const std::string scriptId = body->script_id ? std::string(*body->script_id) : std::string{};
                const std::string generationId =
                    body->generation_id ? std::string(*body->generation_id) : std::string{};
                const std::string cacheKey =
                    body->dialog_cache_key ? std::string(*body->dialog_cache_key) : std::string{};

                if (!isUuidShape(scriptId)) {
                    return bailHttp(span, Status::CODE_400, "script_id must be a UUID");
                }
                if (!isUuidShape(generationId)) {
                    return bailHttp(span, Status::CODE_400, "generation_id must be a UUID");
                }
                if (cacheKey.size() != 64 || cacheKey.find_first_not_of("0123456789abcdef") != std::string::npos) {
                    return bailHttp(span, Status::CODE_400,
                                    "dialog_cache_key must be a 64-character lowercase hex sha256");
                }
                if (span) {
                    span->setAttribute("script.id", scriptId);
                    span->setAttribute("dialog.generation_id", generationId);
                    span->setAttribute("dialog.cache_key", cacheKey);
                }

                auto opSpan =
                    creatures::observability->createChildOperationSpan("DialogVoiceController.acceptVoiceTake", span);

                auto existing = creatures::db->getDialogScript(scriptId, opSpan);
                if (!existing.isSuccess()) {
                    return bailFromServerError(span, existing.getError().value());
                }
                auto script = existing.getValue().value();

                // The take must have been auditioned against the turns as they
                // stand. Accepting against edited turns would store something
                // that reads as stale the instant it lands, which is worse
                // than refusing: it looks like a system fault rather than a
                // sequencing mistake.
                auto currentKey = creatures::voice::computeScriptCacheKey(script.turns, opSpan);
                if (!currentKey.isSuccess()) {
                    return bailFromServerError(span, currentKey.getError().value());
                }
                if (currentKey.getValue().value() != cacheKey) {
                    return bailHttp(span, Status::CODE_400,
                                    "dialog_cache_key does not match the script's current turns — the script was "
                                    "edited after this take was auditioned. Re-audition and accept again.");
                }

                // And the take itself has to still exist in the generation
                // cache under that key.
                auto generation = creatures::voice::loadGeneration(cacheKey, generationId);
                if (!generation.isSuccess()) {
                    return bailHttp(span, Status::CODE_404,
                                    fmt::format("generation '{}' not found for this script's turns (expired or "
                                                "never existed)",
                                                generationId));
                }

                // Replacing: demote the outgoing take first so the sounds
                // directory never briefly holds two.
                if (script.accepted_voice) {
                    if (script.accepted_voice->generation_id == generationId) {
                        // Already accepted — nothing to do, and re-promoting
                        // would look for an ad-hoc file that has already moved.
                        if (span)
                            span->setHttpStatus(200);
                        return createDtoResponse(Status::CODE_200, creatures::convertToDto(script));
                    }
                    auto demoted = creatures::storage::demoteVoiceTake(script.accepted_voice->sound_file,
                                                                       script.accepted_voice->generation_id, opSpan);
                    if (!demoted.isSuccess()) {
                        return bailFromServerError(span, demoted.getError().value());
                    }
                }

                const auto filename =
                    fmt::format("{}-{}.wav", util::slugify(script.title, 48, "dialog"), generationId.substr(0, 8));
                auto promoted = creatures::storage::promoteVoiceTake(generationId, filename, opSpan);
                if (!promoted.isSuccess()) {
                    return bailFromServerError(span, promoted.getError().value());
                }

                creatures::AcceptedVoice accepted;
                accepted.generation_id = generationId;
                accepted.dialog_cache_key = cacheKey;
                accepted.sound_file = promoted.getValue().value().forMetadata;
                accepted.accepted_at = nowMillis();
                script.accepted_voice = accepted;

                auto updated = creatures::dialogScriptToJson(script);
                updated["updated_at"] = std::max(nowMillis(), script.updated_at + 1);
                auto published = creatures::storage::publishDialogScript(updated.dump(), opSpan);
                if (!published.isSuccess()) {
                    return bailFromServerError(span, published.getError().value());
                }

                info("accepted voice take {} for script '{}' -> {}", generationId, script.title, accepted.sound_file);
                if (span) {
                    span->setAttribute("voice.sound_file", accepted.sound_file);
                    span->setHttpStatus(200);
                }
                return createDtoResponse(Status::CODE_200, creatures::convertToDto(published.getValue().value()));
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
