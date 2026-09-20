#include "server/transport/DialogScriptHandlers.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <set>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "api/DialogContracts.h"
#include "api/JsonResponse.h"
#include "model/DialogScript.h"
#include "server/database.h"
#include "server/script/DialogScriptMutationLock.h"
#include "server/storage/Storage.h"
#include "server/transport/HandlerSupport.h"
#include "server/voice/DialogCache.h"
#include "util/ObservabilityManager.h"
#include "util/UuidValidation.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::transport {
namespace {
int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
nlohmann::json canonical(const std::string &body, const std::string &id, const int64_t createdAt,
                         const int64_t updatedAt) {
    auto json = nlohmann::json::parse(body);
    if (!json.is_object())
        throw std::runtime_error("request body must be a JSON object");
    json["id"] = id;
    json["created_at"] = createdAt;
    json["updated_at"] = updatedAt;
    return json;
}
bool validId(const std::string &id, const std::shared_ptr<OperationSpan> &span) {
    if (span)
        span->setAttribute("script.id", id);
    return isUuidShape(id);
}
} // namespace

PreparedResponse listDialogScripts(const std::shared_ptr<OperationSpan> &span) {
    auto operation = childSpan("DialogScriptController.listDialogScripts", span);
    const auto result = creatures::db->listDialogScripts(operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    const auto scripts = result.getValue().value();
    if (operation)
        operation->setSuccess();
    if (span) {
        span->setAttribute("response.items.count", static_cast<int64_t>(scripts.size()));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::listResponseToJson(scripts, dialogScriptToJson)));
}

PreparedResponse getDialogScript(const std::string &scriptId, const std::shared_ptr<OperationSpan> &span) {
    if (!validId(scriptId, span))
        return errorStatus(400, "scriptId must be a UUID", span, "InvalidDialogScriptRequest");
    auto operation = childSpan("DialogScriptController.getDialogScript", span);
    const auto result = creatures::db->getDialogScript(canonicalUuid(scriptId), operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    if (operation)
        operation->setSuccess();
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, dialogScriptToJson(result.getValue().value()).dump());
}

PreparedResponse createDialogScript(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto now = nowMillis();
    nlohmann::json json;
    try {
        json = canonical(body, util::generateUUID(), now, now);
        // Music references and voice acceptances are created only by their
        // own endpoints after promoting the audio; a new script has neither.
        json.erase("background_music");
        json.erase("accepted_voice");
    } catch (const nlohmann::json::exception &error) {
        return errorStatus(400, fmt::format("Invalid JSON: {}", error.what()), span, "InvalidDialogScriptRequest");
    } catch (const std::exception &error) {
        return errorStatus(400, error.what(), span, "InvalidDialogScriptRequest");
    }
    auto operation = childSpan("DialogScriptController.createDialogScript", span);
    const auto parsed = Database::parseDialogScriptJson(json, operation);
    if (!parsed.isSuccess())
        return errorStatus(400, parsed.getError()->getMessage(), span, "InvalidDialogScriptRequest");
    const auto result = storage::publishDialogScript(dialogScriptToJson(parsed.getValue().value()).dump(), operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    if (operation)
        operation->setSuccess();
    if (span) {
        span->setAttribute("script.id", result.getValue()->id);
        span->setSuccess();
    }
    return PreparedResponse::json(201, dialogScriptToJson(result.getValue().value()).dump());
}

PreparedResponse updateDialogScript(const std::string &scriptId, const std::string &body,
                                    const std::shared_ptr<OperationSpan> &span) {
    if (!validId(scriptId, span))
        return errorStatus(400, "scriptId must be a UUID", span, "InvalidDialogScriptRequest");
    const auto id = canonicalUuid(scriptId);
    auto operation = childSpan("DialogScriptController.updateDialogScript", span);
    const std::scoped_lock lock(script::mutationMutex());
    const auto existing = creatures::db->getDialogScript(id, operation);
    if (!existing.isSuccess())
        return serverErrorStatus(existing.getError().value(), span);
    const auto existingScript = existing.getValue().value();
    nlohmann::json json;
    try {
        json = canonical(body, id, existingScript.created_at, nowMillis());
        // The stored music reference and voice acceptance always win over
        // whatever the body claims; only their own endpoints may change them.
        const auto oldJson = dialogScriptToJson(existingScript);
        if (existingScript.background_music)
            json["background_music"] = oldJson["background_music"];
        else
            json.erase("background_music");
        if (existingScript.accepted_voice)
            json["accepted_voice"] = oldJson["accepted_voice"];
        else
            json.erase("accepted_voice");
    } catch (const nlohmann::json::exception &error) {
        return errorStatus(400, fmt::format("Invalid JSON: {}", error.what()), span, "InvalidDialogScriptRequest");
    } catch (const std::exception &error) {
        return errorStatus(400, error.what(), span, "InvalidDialogScriptRequest");
    }
    const auto parsed = Database::parseDialogScriptJson(json, operation);
    if (!parsed.isSuccess())
        return errorStatus(400, parsed.getError()->getMessage(), span, "InvalidDialogScriptRequest");
    const auto result = storage::publishDialogScript(dialogScriptToJson(parsed.getValue().value()).dump(), operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    if (operation)
        operation->setSuccess();
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, dialogScriptToJson(result.getValue().value()).dump());
}

PreparedResponse validateDialogScript(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    api::DialogScriptValidationResponse response;
    {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(body);
        } catch (const std::exception &error) {
            response.valid = false;
            response.errorMessages.push_back(fmt::format("Invalid JSON: {}", error.what()));
            if (span)
                span->setSuccess();
            return PreparedResponse::json(200, api::jsonToString(api::dialogScriptValidationResponseToJson(response)));
        }
        if (!json.is_object()) {
            response.valid = false;
            response.errorMessages.emplace_back("request body must be a JSON object");
            if (span)
                span->setSuccess();
            return PreparedResponse::json(200, api::jsonToString(api::dialogScriptValidationResponseToJson(response)));
        }
        const bool suppliedId = json.contains("id") && json["id"].is_string() && !json["id"].get<std::string>().empty();
        if (!suppliedId)
            json["id"] = "00000000-0000-0000-0000-000000000000";
        auto operation = childSpan("DialogScriptController.validateDialogScript", span);
        const auto parsed = Database::parseDialogScriptJson(json, operation);
        if (!parsed.isSuccess()) {
            response.valid = false;
            response.errorMessages.push_back(parsed.getError()->getMessage());
            if (span)
                span->setSuccess();
            return PreparedResponse::json(200, api::jsonToString(api::dialogScriptValidationResponseToJson(response)));
        }
        const auto script = parsed.getValue().value();
        if (suppliedId)
            response.scriptId = script.id;
        response.turnCount = static_cast<uint32_t>(script.turns.size());
        // Every referenced creature must currently exist. Dedupe so a 50-turn
        // dialog between two creatures doesn't fire 50 lookups, and keep
        // non-UUID input away from the DB layer and its span attributes.
        std::set<std::string> ids;
        for (const auto &turn : script.turns) {
            if (turn.creature_id.empty())
                continue;
            if (!isUuidShape(turn.creature_id)) {
                response.valid = false;
                response.errorMessages.push_back(fmt::format(
                    "turn creature_id is not a UUID: '{}'",
                    turn.creature_id.size() > 64 ? turn.creature_id.substr(0, 64) + "…" : turn.creature_id));
                continue;
            }
            ids.insert(turn.creature_id);
        }
        for (const auto &id : ids) {
            if (!creatures::db->getCreature(id, operation).isSuccess())
                response.missingCreatureIds.push_back(id);
        }
        if (operation)
            operation->setSuccess();
    }
    if (span) {
        span->setAttribute("validation.passed", response.valid);
        span->setAttribute("validation.missing_creature_ids_count",
                           static_cast<int64_t>(response.missingCreatureIds.size()));
        span->setAttribute("validation.turn_count", static_cast<int64_t>(response.turnCount));
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::dialogScriptValidationResponseToJson(response)));
}

PreparedResponse clearDialogMusic(const std::string &scriptId, const std::shared_ptr<OperationSpan> &span) {
    if (!validId(scriptId, span))
        return errorStatus(400, "scriptId must be a UUID", span, "InvalidDialogScriptRequest");
    const auto id = canonicalUuid(scriptId);
    auto operation = childSpan("DialogScriptController.clearDialogBackgroundMusic", span);
    const std::scoped_lock lock(script::mutationMutex());
    const auto existing = creatures::db->getDialogScript(id, operation);
    if (!existing.isSuccess())
        return serverErrorStatus(existing.getError().value(), span);
    const auto script = existing.getValue().value();
    if (!script.background_music) {
        if (span)
            span->setSuccess();
        return PreparedResponse::json(200, dialogScriptToJson(script).dump());
    }
    if (script.updated_at == std::numeric_limits<int64_t>::max())
        return errorStatus(400, "dialog script updated_at cannot advance", span, "InvalidDialogScriptRequest");
    auto json = dialogScriptToJson(script);
    json.erase("background_music");
    json["updated_at"] = std::max(nowMillis(), script.updated_at + 1);
    const auto result = storage::publishDialogScript(json.dump(), operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    if (operation)
        operation->setSuccess();
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, dialogScriptToJson(result.getValue().value()).dump());
}

PreparedResponse clearDialogVoice(const std::string &scriptId, const std::shared_ptr<OperationSpan> &span) {
    if (!validId(scriptId, span))
        return errorStatus(400, "scriptId must be a UUID", span, "InvalidDialogScriptRequest");
    const auto id = canonicalUuid(scriptId);
    auto operation = childSpan("DialogScriptController.clearAcceptedVoice", span);
    const std::scoped_lock lock(script::mutationMutex());
    const auto existing = creatures::db->getDialogScript(id, operation);
    if (!existing.isSuccess())
        return serverErrorStatus(existing.getError().value(), span);
    const auto script = existing.getValue().value();
    if (!script.accepted_voice) {
        if (span)
            span->setSuccess();
        return PreparedResponse::json(200, dialogScriptToJson(script).dump());
    }
    if (script.updated_at == std::numeric_limits<int64_t>::max())
        return errorStatus(400, "dialog script updated_at cannot advance", span, "InvalidDialogScriptRequest");
    auto json = dialogScriptToJson(script);
    json.erase("accepted_voice");
    json["updated_at"] = std::max(nowMillis(), script.updated_at + 1);
    const auto result = storage::publishDialogScript(json.dump(), operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    // Publishing the clear is the commit point. Cleanup follows so a
    // filesystem failure can leave only an unreferenced file, never a script
    // pointing at a WAV that was already moved; the cache entry goes either way.
    const auto demoted =
        storage::demoteVoiceTake(script.accepted_voice->sound_file, script.accepted_voice->generation_id, operation);
    if (!demoted.isSuccess()) {
        spdlog::warn("cleared accepted voice take {} but could not demote its WAV: {}",
                     script.accepted_voice->generation_id, demoted.getError()->getMessage());
    }
    voice::removeAcceptedGeneration(script.accepted_voice->dialog_cache_key, script.accepted_voice->generation_id);
    if (operation)
        operation->setSuccess();
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, dialogScriptToJson(result.getValue().value()).dump());
}

PreparedResponse deleteDialogScript(const std::string &scriptId, const std::shared_ptr<OperationSpan> &span) {
    if (!validId(scriptId, span))
        return errorStatus(400, "scriptId must be a UUID", span, "InvalidDialogScriptRequest");
    auto operation = childSpan("DialogScriptController.deleteDialogScript", span);
    const std::scoped_lock lock(script::mutationMutex());
    const auto result = storage::deleteDialogScript(canonicalUuid(scriptId), operation);
    if (!result.isSuccess())
        return serverErrorStatus(result.getError().value(), span);
    if (operation)
        operation->setSuccess();
    if (span)
        span->setSuccess();
    return PreparedResponse::json(
        200, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(200, "DialogScript deleted"))));
}
} // namespace creatures::transport
