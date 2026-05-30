#pragma once

#include <chrono>
#include <memory>
#include <set>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "model/CacheInvalidation.h"
#include "model/DialogScript.h"
#include "server/config.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/dto/DialogScriptRequestDto.h"
#include "server/ws/dto/DialogScriptValidationDto.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"
#include "util/uuidUtils.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures::ws {

/// REST CRUD surface for saved DialogScripts — the "source code" for
/// multi-character dialog scenes. POST /api/v1/animation/dialog can take a
/// `script_id` and the worker snapshots the script's turns onto the rendered
/// Animation (soft pointer + CoW).
class DialogScriptController : public oatpp::web::server::api::ApiController {
  public:
    DialogScriptController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : ApiController(objectMapper) {}

    static std::shared_ptr<DialogScriptController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                                objectMapper)) {
        return std::make_shared<DialogScriptController>(objectMapper);
    }

  private:
    static int64_t nowMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /// Move a parsed UpsertDialogScriptRequestDto into a DialogScript value the
    /// DB layer accepts. id / timestamps come from the caller (POST stamps them,
    /// PUT preserves created_at from the existing record).
    static creatures::DialogScript fromUpsertDto(const oatpp::Object<UpsertDialogScriptRequestDto> &body,
                                                 const std::string &id, int64_t createdAt, int64_t updatedAt) {
        creatures::DialogScript script;
        script.id = id;
        script.title = body->title ? std::string(*body->title) : std::string{};
        script.notes = body->notes ? std::string(*body->notes) : std::string{};
        if (body->turns) {
            for (const auto &t : *body->turns) {
                if (!t)
                    continue;
                creatures::DialogScriptTurn turn;
                if (t->creature_id)
                    turn.creature_id = t->creature_id;
                if (t->text)
                    turn.text = t->text;
                script.turns.push_back(std::move(turn));
            }
        }
        script.created_at = createdAt;
        script.updated_at = updatedAt;
        return script;
    }

    /// Reject the obvious shape errors before we touch the DB. Returns nullptr
    /// on success; an HTTP error response otherwise.
    template <typename SpanT>
    std::shared_ptr<OutgoingResponse> validateUpsertBody(const oatpp::Object<UpsertDialogScriptRequestDto> &body,
                                                         const SpanT &span) {
        auto bail = [&](const char *msg) -> std::shared_ptr<OutgoingResponse> {
            auto err = StatusDto::createShared();
            err->status = "error";
            err->code = 400;
            err->message = msg;
            if (span)
                span->setHttpStatus(400);
            return createDtoResponse(Status::CODE_400, err);
        };
        if (!body) {
            return bail("Request body is required");
        }
        // Enforce the same caps as the DB-layer parser so an oversized payload
        // doesn't get round-tripped through dialogScriptToJson().dump() before
        // dying (security review S3). Shared constants live in
        // model/DialogScript.h.
        if (!body->title || body->title->empty()) {
            return bail("title is required and must be non-empty");
        }
        if (body->title->size() > creatures::MAX_DIALOG_SCRIPT_TITLE) {
            return bail(
                fmt::format("title is {} chars; max {}", body->title->size(), creatures::MAX_DIALOG_SCRIPT_TITLE)
                    .c_str());
        }
        if (body->notes && body->notes->size() > creatures::MAX_DIALOG_SCRIPT_NOTES) {
            return bail(
                fmt::format("notes is {} chars; max {}", body->notes->size(), creatures::MAX_DIALOG_SCRIPT_NOTES)
                    .c_str());
        }
        if (!body->turns || body->turns->empty()) {
            return bail("turns is required and must be non-empty");
        }
        if (body->turns->size() > creatures::MAX_DIALOG_SCRIPT_TURNS) {
            return bail(
                fmt::format("turns has {} entries; max {}", body->turns->size(), creatures::MAX_DIALOG_SCRIPT_TURNS)
                    .c_str());
        }
        for (const auto &t : *body->turns) {
            if (!t || !t->creature_id || t->creature_id->empty() || !t->text || t->text->empty()) {
                return bail("every turn must have a non-empty creature_id and text");
            }
            if (t->text->size() > creatures::MAX_DIALOG_SCRIPT_TURN_TEXT) {
                return bail(fmt::format("turn 'text' is {} chars; max {}", t->text->size(),
                                        creatures::MAX_DIALOG_SCRIPT_TURN_TEXT)
                                .c_str());
            }
        }
        return nullptr;
    }

  public:
    ENDPOINT_INFO(listDialogScripts) {
        info->summary = "List all saved dialog scripts (newest first by updated_at)";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<ListDto<Object<DialogScriptDto>>>>(Status::CODE_200,
                                                                    "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/dialog/script", listDialogScripts,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/dialog/script", "GET", "api/v1/animation/dialog/script",
                           "listDialogScripts", "DialogScriptController", request,
                           [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "DialogScriptController.listDialogScripts", span);
                               auto result = creatures::db->listDialogScripts(opSpan);
                               if (!result.isSuccess()) {
                                   auto err = StatusDto::createShared();
                                   err->status = "error";
                                   err->code = 500;
                                   err->message = result.getError().value().getMessage().c_str();
                                   if (span)
                                       span->setHttpStatus(500);
                                   return createDtoResponse(Status::CODE_500, err);
                               }
                               const auto scripts = result.getValue().value();
                               auto list = ListDto<Object<DialogScriptDto>>::createShared();
                               list->count = static_cast<v_uint32>(scripts.size());
                               list->items = oatpp::Vector<Object<DialogScriptDto>>::createShared();
                               for (const auto &s : scripts) {
                                   list->items->push_back(creatures::convertToDto(s));
                               }
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, list);
                           });
    }

    ENDPOINT_INFO(getDialogScript) {
        info->summary = "Fetch one saved dialog script by id";
        info->addTag("Multi-character Dialog");
        info->pathParams["scriptId"].description = "DialogScript UUID";
        info->addResponse<Object<DialogScriptDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/dialog/script/{scriptId}", getDialogScript, PATH(String, scriptId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/dialog/script/{scriptId}", "GET", "api/v1/animation/dialog/script/{scriptId}",
            "getDialogScript", "DialogScriptController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                OATPP_ASSERT_HTTP(scriptId && isUuidShape(std::string(*scriptId)), Status::CODE_400,
                                  "scriptId must be a UUID");
                if (span)
                    span->setAttribute("script.id", std::string(*scriptId));
                auto opSpan =
                    creatures::observability->createChildOperationSpan("DialogScriptController.getDialogScript", span);
                auto result = creatures::db->getDialogScript(std::string(*scriptId), opSpan);
                if (!result.isSuccess()) {
                    auto code = result.getError().value().getCode();
                    const bool notFound = code == creatures::ServerError::NotFound;
                    auto err = StatusDto::createShared();
                    err->status = notFound ? "not_found" : "error";
                    err->code = notFound ? 404 : 500;
                    err->message = result.getError().value().getMessage().c_str();
                    if (span)
                        span->setHttpStatus(notFound ? 404 : 500);
                    return createDtoResponse(notFound ? Status::CODE_404 : Status::CODE_500, err);
                }
                if (span)
                    span->setHttpStatus(200);
                return createDtoResponse(Status::CODE_200, creatures::convertToDto(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(createDialogScript) {
        info->summary = "Create a new saved dialog script";
        info->description = "Server generates the script's UUID and stamps created_at + updated_at. Returns the "
                            "stored script with its new id.";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<DialogScriptDto>>(Status::CODE_201, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/script", createDialogScript,
             BODY_DTO(Object<UpsertDialogScriptRequestDto>, body), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/script", "POST", "api/v1/animation/dialog/script", "createDialogScript",
            "DialogScriptController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (auto err = validateUpsertBody(body, span))
                    return err;
                const auto now = nowMillis();
                const auto id = util::generateUUID();
                auto script = fromUpsertDto(body, id, now, now);
                const auto j = dialogScriptToJson(script);

                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogScriptController.createDialogScript", span);
                auto result = creatures::db->upsertDialogScript(j.dump(), opSpan);
                if (!result.isSuccess()) {
                    auto code = result.getError().value().getCode();
                    const bool client = code == creatures::ServerError::InvalidData;
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = client ? 400 : 500;
                    err->message = result.getError().value().getMessage().c_str();
                    if (span)
                        span->setHttpStatus(client ? 400 : 500);
                    return createDtoResponse(client ? Status::CODE_400 : Status::CODE_500, err);
                }
                scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, creatures::CacheType::DialogScriptList);
                if (span) {
                    span->setAttribute("script.id", result.getValue().value().id);
                    span->setHttpStatus(201);
                }
                return createDtoResponse(Status::CODE_201, creatures::convertToDto(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(updateDialogScript) {
        info->summary = "Update (replace) an existing dialog script";
        info->description = "Body replaces the script's title/notes/turns; id comes from the URL. created_at is "
                            "preserved from the existing record; updated_at gets bumped to now. Returns 404 if no "
                            "script with that id exists.";
        info->addTag("Multi-character Dialog");
        info->pathParams["scriptId"].description = "DialogScript UUID";
        info->addResponse<Object<DialogScriptDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("PUT", "api/v1/animation/dialog/script/{scriptId}", updateDialogScript, PATH(String, scriptId),
             BODY_DTO(Object<UpsertDialogScriptRequestDto>, body), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "PUT /api/v1/animation/dialog/script/{scriptId}", "PUT", "api/v1/animation/dialog/script/{scriptId}",
            "updateDialogScript", "DialogScriptController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                OATPP_ASSERT_HTTP(scriptId && isUuidShape(std::string(*scriptId)), Status::CODE_400,
                                  "scriptId must be a UUID");
                if (span)
                    span->setAttribute("script.id", std::string(*scriptId));
                if (auto err = validateUpsertBody(body, span))
                    return err;

                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogScriptController.updateDialogScript", span);

                // Must exist — PUT replaces, not creates-via-id.
                auto existing = creatures::db->getDialogScript(std::string(*scriptId), opSpan);
                if (!existing.isSuccess()) {
                    auto code = existing.getError().value().getCode();
                    const bool notFound = code == creatures::ServerError::NotFound;
                    auto err = StatusDto::createShared();
                    err->status = notFound ? "not_found" : "error";
                    err->code = notFound ? 404 : 500;
                    err->message = existing.getError().value().getMessage().c_str();
                    if (span)
                        span->setHttpStatus(notFound ? 404 : 500);
                    return createDtoResponse(notFound ? Status::CODE_404 : Status::CODE_500, err);
                }
                const auto createdAt = existing.getValue().value().created_at;

                auto script = fromUpsertDto(body, std::string(*scriptId), createdAt, nowMillis());
                const auto j = dialogScriptToJson(script);
                auto result = creatures::db->upsertDialogScript(j.dump(), opSpan);
                if (!result.isSuccess()) {
                    auto code = result.getError().value().getCode();
                    const bool client = code == creatures::ServerError::InvalidData;
                    auto err = StatusDto::createShared();
                    err->status = "error";
                    err->code = client ? 400 : 500;
                    err->message = result.getError().value().getMessage().c_str();
                    if (span)
                        span->setHttpStatus(client ? 400 : 500);
                    return createDtoResponse(client ? Status::CODE_400 : Status::CODE_500, err);
                }
                scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, creatures::CacheType::DialogScriptList);
                if (span)
                    span->setHttpStatus(200);
                return createDtoResponse(Status::CODE_200, creatures::convertToDto(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(validateDialogScript) {
        info->summary = "Validate a dialog script payload without saving it";
        info->description = "Shape-only check: parses the JSON, runs the same field-level validation the upsert path "
                            "uses, and soft-checks that every turn's creature_id exists. Returns 200 with valid=true "
                            "or valid=false + error_messages — never throws, so the client can render inline form "
                            "errors without exception handling. id, created_at, and updated_at are tolerated if "
                            "present (the client may send a round-tripped DialogScriptDto) but are not required.";
        info->addTag("Multi-character Dialog");
        info->addResponse<Object<DialogScriptValidationDto>>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/script/validate", validateDialogScript, BODY_STRING(String, body),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/script/validate", "POST", "api/v1/animation/dialog/script/validate",
            "validateDialogScript", "DialogScriptController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                auto resultDto = DialogScriptValidationDto::createShared();
                resultDto->valid = true;
                resultDto->turn_count = static_cast<v_uint32>(0);
                resultDto->missing_creature_ids = oatpp::List<oatpp::String>::createShared();
                resultDto->error_messages = oatpp::List<oatpp::String>::createShared();

                const std::string raw = body ? std::string(*body) : std::string{};
                nlohmann::json parsed;
                try {
                    parsed = nlohmann::json::parse(raw);
                } catch (const std::exception &ex) {
                    resultDto->valid = false;
                    resultDto->error_messages->push_back(fmt::format("Invalid JSON: {}", ex.what()).c_str());
                    if (span)
                        span->setHttpStatus(200);
                    return createDtoResponse(Status::CODE_200, resultDto);
                }

                // parseDialogScriptJson requires `id` since it's also used by the upsert
                // path. For validate, the client may not have an id yet (create flow) —
                // stamp a placeholder so the schema check covers everything else. We
                // strip it before reporting script_id back.
                const bool clientProvidedId =
                    parsed.contains("id") && parsed["id"].is_string() && !parsed["id"].get<std::string>().empty();
                if (!clientProvidedId) {
                    parsed["id"] = "00000000-0000-0000-0000-000000000000";
                }

                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogScriptController.validateDialogScript", span);
                auto parseResult = creatures::Database::parseDialogScriptJson(parsed, opSpan);
                if (!parseResult.isSuccess()) {
                    resultDto->valid = false;
                    resultDto->error_messages->push_back(parseResult.getError()->getMessage().c_str());
                    if (span)
                        span->setHttpStatus(200);
                    return createDtoResponse(Status::CODE_200, resultDto);
                }
                const auto script = parseResult.getValue().value();
                if (clientProvidedId) {
                    resultDto->script_id = script.id.c_str();
                }
                resultDto->turn_count = static_cast<v_uint32>(script.turns.size());

                // Soft warning: every creature_id the script references must currently
                // exist on the server. Dedupe so a 50-turn dialog between two creatures
                // doesn't fire 50 DB lookups. Pre-filter through isUuidShape so non-UUID
                // attacker input never reaches the DB layer or its span attributes
                // (security review S4).
                std::set<std::string> uniqueCreatureIds;
                for (const auto &t : script.turns) {
                    if (t.creature_id.empty())
                        continue;
                    if (!isUuidShape(t.creature_id)) {
                        resultDto->valid = false;
                        resultDto->error_messages->push_back(
                            fmt::format("turn creature_id is not a UUID: '{}'",
                                        t.creature_id.size() > 64 ? t.creature_id.substr(0, 64) + "…" : t.creature_id)
                                .c_str());
                        continue;
                    }
                    uniqueCreatureIds.insert(t.creature_id);
                }
                for (const auto &cid : uniqueCreatureIds) {
                    auto creatureLookup = creatures::db->getCreature(cid, opSpan);
                    if (!creatureLookup.isSuccess()) {
                        resultDto->missing_creature_ids->push_back(cid.c_str());
                    }
                }

                if (span) {
                    span->setAttribute("validation.passed", static_cast<bool>(resultDto->valid));
                    span->setAttribute("validation.missing_creature_ids_count",
                                       static_cast<int64_t>(resultDto->missing_creature_ids->size()));
                    span->setAttribute("validation.turn_count", static_cast<int64_t>(*resultDto->turn_count));
                    span->setHttpStatus(200);
                }
                return createDtoResponse(Status::CODE_200, resultDto);
            });
    }

    ENDPOINT_INFO(deleteDialogScript) {
        info->summary = "Delete a saved dialog script";
        info->description = "Animations rendered from this script aren't touched — they carry a CoW snapshot of the "
                            "turns in their metadata, so they remain playable and traceable.";
        info->addTag("Multi-character Dialog");
        info->pathParams["scriptId"].description = "DialogScript UUID";
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("DELETE", "api/v1/animation/dialog/script/{scriptId}", deleteDialogScript, PATH(String, scriptId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("DELETE /api/v1/animation/dialog/script/{scriptId}", "DELETE",
                           "api/v1/animation/dialog/script/{scriptId}", "deleteDialogScript", "DialogScriptController",
                           request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               OATPP_ASSERT_HTTP(scriptId && isUuidShape(std::string(*scriptId)), Status::CODE_400,
                                                 "scriptId must be a UUID");
                               if (span)
                                   span->setAttribute("script.id", std::string(*scriptId));
                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "DialogScriptController.deleteDialogScript", span);
                               auto result = creatures::db->deleteDialogScript(std::string(*scriptId), opSpan);
                               if (!result.isSuccess()) {
                                   auto code = result.getError().value().getCode();
                                   const bool notFound = code == creatures::ServerError::NotFound;
                                   auto err = StatusDto::createShared();
                                   err->status = notFound ? "not_found" : "error";
                                   err->code = notFound ? 404 : 500;
                                   err->message = result.getError().value().getMessage().c_str();
                                   if (span)
                                       span->setHttpStatus(notFound ? 404 : 500);
                                   return createDtoResponse(notFound ? Status::CODE_404 : Status::CODE_500, err);
                               }
                               scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME,
                                                              creatures::CacheType::DialogScriptList);
                               auto ok = StatusDto::createShared();
                               ok->status = "OK";
                               ok->code = 200;
                               ok->message = "DialogScript deleted";
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, ok);
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
