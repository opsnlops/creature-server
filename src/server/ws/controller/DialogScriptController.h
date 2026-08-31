#pragma once

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/DialogContracts.h"
#include "api/JsonResponse.h"
#include "model/CacheInvalidation.h"
#include "model/DialogScript.h"
#include "server/config.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/script/DialogScriptMutationLock.h"
#include "server/storage/Storage.h"
#include "server/voice/DialogCache.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
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
class DialogScriptController : public oatpp::web::server::api::ApiController,
                               public HttpResponseHelpers<DialogScriptController> {
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

    /// Build the canonical script JSON from raw client input: parse with
    /// nlohmann (lenient — extras are silently ignored), then stamp the
    /// server-managed fields (id / created_at / updated_at) on top. The result
    /// is what `parseDialogScriptJson` expects.
    ///
    /// Parsing into neutral JSON matters for client UX: extras are silently
    /// dropped so clients can round-trip a full script response, while
    /// structural problems surface with friendly, field-specific messages
    /// from invalidScriptData (security review S3 caps live there).
    static nlohmann::json buildScriptJsonForUpsert(const std::string &rawBody, const std::string &id, int64_t createdAt,
                                                   int64_t updatedAt) {
        auto parsed = nlohmann::json::parse(rawBody); // throws on bad JSON; caller catches.
        if (!parsed.is_object()) {
            throw std::runtime_error("request body must be a JSON object");
        }
        // Stamp the server-managed fields. Any client-supplied values get overwritten.
        parsed["id"] = id;
        parsed["created_at"] = createdAt;
        parsed["updated_at"] = updatedAt;
        return parsed;
    }

  public:
    ENDPOINT_INFO(listDialogScripts) {
        info->summary = "List all saved dialog scripts (newest first by updated_at)";
        info->addTag("Multi-character Dialog");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
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
                                   return bailFromServerError(span, result.getError().value());
                               }
                               const auto scripts = result.getValue().value();
                               if (span) {
                                   span->setAttribute("response.items.count", static_cast<int64_t>(scripts.size()));
                                   span->setHttpStatus(200);
                               }
                               return jsonResponse(span, Status::CODE_200,
                                                   api::listResponseToJson(scripts, creatures::dialogScriptToJson));
                           });
    }

    ENDPOINT_INFO(getDialogScript) {
        info->summary = "Fetch one saved dialog script by id";
        info->addTag("Multi-character Dialog");
        info->pathParams["scriptId"].description = "DialogScript UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/dialog/script/{scriptId}", getDialogScript, PATH(String, scriptId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/animation/dialog/script/{scriptId}", "GET", "api/v1/animation/dialog/script/{scriptId}",
            "getDialogScript", "DialogScriptController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!scriptId || !isUuidShape(std::string(*scriptId))) {
                    return bailHttp(span, Status::CODE_400, "scriptId must be a UUID");
                }
                const auto id = canonicalUuid(std::string(*scriptId));
                if (span)
                    span->setAttribute("script.id", id);
                auto opSpan =
                    creatures::observability->createChildOperationSpan("DialogScriptController.getDialogScript", span);
                auto result = creatures::db->getDialogScript(id, opSpan);
                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, creatures::dialogScriptToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(createDialogScript) {
        info->summary = "Create a new saved dialog script";
        info->description = "Server generates the script's UUID and stamps created_at + updated_at. Returns the "
                            "stored script with its new id.";
        info->addTag("Multi-character Dialog");
        info->addResponse<oatpp::String>(Status::CODE_201, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/script", createDialogScript,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/script", "POST", "api/v1/animation/dialog/script", "createDialogScript",
            "DialogScriptController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                const auto body = readRequestBodyLimited(request, api::MAX_DIALOG_REQUEST_BYTES, span);

                const auto now = nowMillis();
                const auto id = util::generateUUID();
                nlohmann::json parsed;
                try {
                    parsed = buildScriptJsonForUpsert(body, id, now, now);
                    // Music references are created only by the promotion endpoint,
                    // after it has verified the permanent WAV and embedded recipe;
                    // an acceptance likewise only comes from the accept endpoint,
                    // which promotes the audio first. A brand new script has
                    // neither, whatever the body claims.
                    parsed.erase("background_music");
                    parsed.erase("accepted_voice");
                } catch (const nlohmann::json::exception &e) {
                    return bailHttp(span, Status::CODE_400, fmt::format("Invalid JSON: {}", e.what()));
                } catch (const std::exception &e) {
                    return bailHttp(span, Status::CODE_400, e.what());
                }

                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogScriptController.createDialogScript", span);

                // Field-level validation (caps, types, UUID shape via invalidScriptData).
                auto parseResult = creatures::Database::parseDialogScriptJson(parsed, opSpan);
                if (!parseResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_400, parseResult.getError()->getMessage());
                }

                // Persist the parsed model, not the original client document.
                // This is the allowlist boundary that drops tolerated unknown
                // fields instead of retaining arbitrary JSON in MongoDB.
                auto result = creatures::storage::publishDialogScript(
                    creatures::dialogScriptToJson(parseResult.getValue().value()).dump(), opSpan);
                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }
                if (span) {
                    span->setAttribute("script.id", result.getValue().value().id);
                    span->setHttpStatus(201);
                }
                return jsonResponse(span, Status::CODE_201, creatures::dialogScriptToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(updateDialogScript) {
        info->summary = "Update (replace) an existing dialog script";
        info->description = "Body replaces the script's title/notes/turns; id comes from the URL. created_at is "
                            "preserved from the existing record; updated_at gets bumped to now. Returns 404 if no "
                            "script with that id exists.";
        info->addTag("Multi-character Dialog");
        info->pathParams["scriptId"].description = "DialogScript UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("PUT", "api/v1/animation/dialog/script/{scriptId}", updateDialogScript, PATH(String, scriptId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("PUT /api/v1/animation/dialog/script/{scriptId}", "PUT",
                           "api/v1/animation/dialog/script/{scriptId}", "updateDialogScript", "DialogScriptController",
                           request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               if (!scriptId || !isUuidShape(std::string(*scriptId))) {
                                   return bailHttp(span, Status::CODE_400, "scriptId must be a UUID");
                               }
                               const auto id = canonicalUuid(std::string(*scriptId));
                               if (span)
                                   span->setAttribute("script.id", id);
                               const auto body = readRequestBodyLimited(request, api::MAX_DIALOG_REQUEST_BYTES, span);

                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "DialogScriptController.updateDialogScript", span);
                               const std::scoped_lock mutationLock(creatures::script::mutationMutex());

                               // Must exist — PUT replaces, not creates-via-id.
                               auto existing = creatures::db->getDialogScript(id, opSpan);
                               if (!existing.isSuccess()) {
                                   return bailFromServerError(span, existing.getError().value());
                               }
                               const auto existingScript = existing.getValue().value();
                               const auto createdAt = existingScript.created_at;

                               nlohmann::json parsed;
                               try {
                                   parsed = buildScriptJsonForUpsert(body, id, createdAt, nowMillis());
                                   // Accepted music and the accepted voice take are server-managed:
                                   // an ordinary script edit must neither forge one nor detach one.
                                   // Carrying them forward is now load-bearing rather than tidy —
                                   // the upsert replaces the document (#134), so anything not
                                   // written here is gone.
                                   const auto existingJson = creatures::dialogScriptToJson(existingScript);
                                   if (existingScript.background_music) {
                                       parsed["background_music"] = existingJson["background_music"];
                                   } else {
                                       parsed.erase("background_music");
                                   }
                                   if (existingScript.accepted_voice) {
                                       parsed["accepted_voice"] = existingJson["accepted_voice"];
                                   } else {
                                       parsed.erase("accepted_voice");
                                   }
                               } catch (const nlohmann::json::exception &e) {
                                   return bailHttp(span, Status::CODE_400, fmt::format("Invalid JSON: {}", e.what()));
                               } catch (const std::exception &e) {
                                   return bailHttp(span, Status::CODE_400, e.what());
                               }

                               auto parseResult = creatures::Database::parseDialogScriptJson(parsed, opSpan);
                               if (!parseResult.isSuccess()) {
                                   return bailHttp(span, Status::CODE_400, parseResult.getError()->getMessage());
                               }

                               auto result = creatures::storage::publishDialogScript(
                                   creatures::dialogScriptToJson(parseResult.getValue().value()).dump(), opSpan);
                               if (!result.isSuccess()) {
                                   return bailFromServerError(span, result.getError().value());
                               }
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200,
                                                   creatures::dialogScriptToJson(result.getValue().value()));
                           });
    }

    ENDPOINT_INFO(clearDialogBackgroundMusic) {
        info->summary = "Clear the accepted background music from a saved dialog script";
        info->description =
            "Detaches the accepted background-music reference so future renders are dialog-only. The permanent "
            "WAV/MP3 assets are retained, and the updated script is returned.";
        info->addTag("Multi-character Dialog");
        info->pathParams["scriptId"].description = "DialogScript UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("DELETE", "api/v1/animation/dialog/script/{scriptId}/music", clearDialogBackgroundMusic,
             PATH(String, scriptId), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "DELETE /api/v1/animation/dialog/script/{scriptId}/music", "DELETE",
            "api/v1/animation/dialog/script/{scriptId}/music", "clearDialogBackgroundMusic", "DialogScriptController",
            request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!scriptId || !isUuidShape(std::string(*scriptId))) {
                    return bailHttp(span, Status::CODE_400, "scriptId must be a UUID");
                }
                const auto id = canonicalUuid(std::string(*scriptId));
                if (span)
                    span->setAttribute("script.id", id);

                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogScriptController.clearDialogBackgroundMusic", span);
                const std::scoped_lock mutationLock(creatures::script::mutationMutex());

                const auto setAttribute = [&](const std::string &key, const auto &value) {
                    if (span)
                        span->setAttribute(key, value);
                    if (opSpan)
                        opSpan->setAttribute(key, value);
                };
                setAttribute("script.id", id);
                setAttribute("cache.invalidation.type", "dialog-script-list");

                const auto fail = [&](const ServerError &error, const char *outcome) {
                    setAttribute("music.clear_outcome", outcome);
                    setAttribute("cache.invalidation_requested", false);
                    recordSpanError(opSpan, error.getMessage(), "DialogScriptClearError", error.getCode());
                    return bailFromServerError(span, error);
                };

                auto existing = creatures::db->getDialogScript(id, opSpan);
                if (!existing.isSuccess()) {
                    return fail(existing.getError().value(), "read_error");
                }
                const auto existingScript = existing.getValue().value();
                setAttribute("music.was_attached", existingScript.background_music.has_value());
                setAttribute("music.assets_preserved", true);
                if (existingScript.updated_at == std::numeric_limits<int64_t>::max()) {
                    return fail(ServerError(ServerError::InvalidData, "dialog script updated_at cannot advance"),
                                "invalid_timestamp");
                }

                // Make the operation idempotent. A second clear returns the
                // canonical script without bumping updated_at or broadcasting
                // a redundant cache invalidation.
                if (!existingScript.background_music) {
                    setAttribute("music.cleared", false);
                    setAttribute("music.clear_outcome", "already_clear");
                    setAttribute("cache.invalidation_requested", false);
                    if (opSpan)
                        opSpan->setSuccess();
                    if (span)
                        span->setHttpStatus(200);
                    return jsonResponse(span, Status::CODE_200, creatures::dialogScriptToJson(existingScript));
                }

                auto updated = creatures::dialogScriptToJson(existingScript);
                updated.erase("background_music");
                // updated_at is also used as the render-candidate revision. Keep
                // it strictly increasing even when two writes land in one clock
                // millisecond, so a clear cannot be mistaken for the old script.
                updated["updated_at"] = std::max(nowMillis(), existingScript.updated_at + 1);
                auto published = creatures::storage::publishDialogScript(updated.dump(), opSpan);
                if (!published.isSuccess()) {
                    return fail(published.getError().value(), "publish_error");
                }
                setAttribute("music.cleared", true);
                setAttribute("music.clear_outcome", "cleared");
                setAttribute("cache.invalidation_requested", true);
                if (opSpan)
                    opSpan->setSuccess();
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200,
                                    creatures::dialogScriptToJson(published.getValue().value()));
            });
    }

    ENDPOINT_INFO(validateDialogScript) {
        info->summary = "Validate a dialog script payload without saving it";
        info->description = "Shape-only check: parses the JSON, runs the same field-level validation the upsert path "
                            "uses, and soft-checks that every turn's creature_id exists. Returns 200 with valid=true "
                            "or valid=false + error_messages — never throws, so the client can render inline form "
                            "errors without exception handling. id, created_at, and updated_at are tolerated if "
                            "present (the client may send a round-tripped script response) but are not required.";
        info->addTag("Multi-character Dialog");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/dialog/script/validate", validateDialogScript,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/dialog/script/validate", "POST", "api/v1/animation/dialog/script/validate",
            "validateDialogScript", "DialogScriptController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                api::DialogScriptValidationResponse validationResponse;
                const auto raw = readRequestBodyLimited(request, api::MAX_DIALOG_REQUEST_BYTES, span);
                nlohmann::json parsed;
                try {
                    parsed = nlohmann::json::parse(raw);
                } catch (const std::exception &ex) {
                    validationResponse.valid = false;
                    validationResponse.errorMessages.push_back(fmt::format("Invalid JSON: {}", ex.what()));
                    if (span)
                        span->setHttpStatus(200);
                    return jsonResponse(span, Status::CODE_200,
                                        api::dialogScriptValidationResponseToJson(validationResponse));
                }
                if (!parsed.is_object()) {
                    validationResponse.valid = false;
                    validationResponse.errorMessages.emplace_back("request body must be a JSON object");
                    if (span)
                        span->setHttpStatus(200);
                    return jsonResponse(span, Status::CODE_200,
                                        api::dialogScriptValidationResponseToJson(validationResponse));
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
                    validationResponse.valid = false;
                    validationResponse.errorMessages.push_back(parseResult.getError()->getMessage());
                    if (span)
                        span->setHttpStatus(200);
                    return jsonResponse(span, Status::CODE_200,
                                        api::dialogScriptValidationResponseToJson(validationResponse));
                }
                const auto script = parseResult.getValue().value();
                if (clientProvidedId) {
                    validationResponse.scriptId = script.id;
                }
                validationResponse.turnCount = static_cast<uint32_t>(script.turns.size());

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
                        validationResponse.valid = false;
                        validationResponse.errorMessages.push_back(
                            fmt::format("turn creature_id is not a UUID: '{}'",
                                        t.creature_id.size() > 64 ? t.creature_id.substr(0, 64) + "…" : t.creature_id));
                        continue;
                    }
                    uniqueCreatureIds.insert(t.creature_id);
                }
                for (const auto &cid : uniqueCreatureIds) {
                    auto creatureLookup = creatures::db->getCreature(cid, opSpan);
                    if (!creatureLookup.isSuccess()) {
                        validationResponse.missingCreatureIds.push_back(cid);
                    }
                }

                if (span) {
                    span->setAttribute("validation.passed", validationResponse.valid);
                    span->setAttribute("validation.missing_creature_ids_count",
                                       static_cast<int64_t>(validationResponse.missingCreatureIds.size()));
                    span->setAttribute("validation.turn_count", static_cast<int64_t>(validationResponse.turnCount));
                    span->setHttpStatus(200);
                }
                return jsonResponse(span, Status::CODE_200,
                                    api::dialogScriptValidationResponseToJson(validationResponse));
            });
    }

    ENDPOINT_INFO(clearAcceptedVoice) {
        info->summary = "Clear the script's accepted voice take";
        info->description =
            "Un-accepts the chosen take (#131). The promoted audio is DEMOTED back to the ad-hoc bucket, which "
            "restarts its 24h TTL and gives a change-your-mind window before the sweep reclaims it.\n\n"
            "Idempotent: clearing a script that has no acceptance returns the script unchanged without bumping "
            "updated_at or broadcasting.";
        info->addTag("Multi-character Dialog");
        info->pathParams["scriptId"].description = "Dialog script UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("DELETE", "api/v1/animation/dialog/script/{scriptId}/voice", clearAcceptedVoice, PATH(String, scriptId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "DELETE /api/v1/animation/dialog/script/{scriptId}/voice", "DELETE",
            "api/v1/animation/dialog/script/{scriptId}/voice", "clearAcceptedVoice", "DialogScriptController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!scriptId || !isUuidShape(std::string(*scriptId))) {
                    return bailHttp(span, Status::CODE_400, "scriptId must be a UUID");
                }
                auto opSpan = creatures::observability->createChildOperationSpan(
                    "DialogScriptController.clearAcceptedVoice", span);
                const std::scoped_lock mutationLock(creatures::script::mutationMutex());
                const auto id = canonicalUuid(std::string(*scriptId));
                if (span)
                    span->setAttribute("script.id", id);
                auto existing = creatures::db->getDialogScript(id, opSpan);
                if (!existing.isSuccess()) {
                    return bailFromServerError(span, existing.getError().value());
                }
                auto existingScript = existing.getValue().value();
                if (span) {
                    span->setAttribute("voice.was_accepted", existingScript.accepted_voice.has_value());
                }

                // Nothing accepted — return the script untouched rather than
                // bumping updated_at for a no-op.
                if (!existingScript.accepted_voice) {
                    if (span)
                        span->setHttpStatus(200);
                    return jsonResponse(span, Status::CODE_200, creatures::dialogScriptToJson(existingScript));
                }
                if (existingScript.updated_at == std::numeric_limits<int64_t>::max()) {
                    return bailHttp(span, Status::CODE_400, "dialog script updated_at cannot advance");
                }

                auto updated = creatures::dialogScriptToJson(existingScript);
                updated.erase("accepted_voice");
                updated["updated_at"] = std::max(nowMillis(), existingScript.updated_at + 1);
                auto published = creatures::storage::publishDialogScript(updated.dump(), opSpan);
                if (!published.isSuccess()) {
                    return bailFromServerError(span, published.getError().value());
                }

                // Publishing the clear is the commit point. Cleanup follows
                // so a filesystem failure can leave only an unreferenced file,
                // never a script pointing at a WAV that was already moved.
                auto demoted = creatures::storage::demoteVoiceTake(
                    existingScript.accepted_voice->sound_file, existingScript.accepted_voice->generation_id, opSpan);
                if (!demoted.isSuccess()) {
                    warn("cleared accepted voice take {} but could not demote its WAV: {}",
                         existingScript.accepted_voice->generation_id, demoted.getError()->getMessage());
                }
                creatures::voice::removeAcceptedGeneration(existingScript.accepted_voice->dialog_cache_key,
                                                           existingScript.accepted_voice->generation_id);
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200,
                                    creatures::dialogScriptToJson(published.getValue().value()));
            });
    }

    ENDPOINT_INFO(deleteDialogScript) {
        info->summary = "Delete a saved dialog script";
        info->description = "Animations rendered from this script aren't touched — they carry a CoW snapshot of the "
                            "turns in their metadata, so they remain playable and traceable.";
        info->addTag("Multi-character Dialog");
        info->pathParams["scriptId"].description = "DialogScript UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("DELETE", "api/v1/animation/dialog/script/{scriptId}", deleteDialogScript, PATH(String, scriptId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("DELETE /api/v1/animation/dialog/script/{scriptId}", "DELETE",
                           "api/v1/animation/dialog/script/{scriptId}", "deleteDialogScript", "DialogScriptController",
                           request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               if (!scriptId || !isUuidShape(std::string(*scriptId))) {
                                   return bailHttp(span, Status::CODE_400, "scriptId must be a UUID");
                               }
                               const auto id = canonicalUuid(std::string(*scriptId));
                               if (span)
                                   span->setAttribute("script.id", id);
                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "DialogScriptController.deleteDialogScript", span);
                               const std::scoped_lock mutationLock(creatures::script::mutationMutex());
                               auto result = creatures::storage::deleteDialogScript(id, opSpan);
                               if (!result.isSuccess()) {
                                   return bailFromServerError(span, result.getError().value());
                               }
                               return okStatus(span, Status::CODE_200, "DialogScript deleted");
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
