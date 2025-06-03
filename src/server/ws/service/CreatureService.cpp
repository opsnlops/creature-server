#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "util/Result.h"
#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"

#include "server/ws/dto/ListDto.h"
#include "util/ObservabilityManager.h" // Include ObservabilityManager

#include "CreatureService.h"


namespace creatures {
    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObservabilityManager> observability; // Declare observability extern
}


namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;

    oatpp::Object<ListDto<oatpp::Object<creatures::CreatureDto>>> CreatureService::getAllCreatures(std::shared_ptr<RequestSpan> parentSpan) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        if (!parentSpan) {
            warn("no parent span provided for CreatureService.getAllCreatures, creating a root span");
        }

        // 🐰 Create a trace span for this request
        auto span = creatures::observability->createOperationSpan("CreatureService.getAllCreatures",
            std::move(parentSpan));

        appLogger->debug("CreatureService::getAllCreatures()");

        if (span) {
            trace("adding attributes to the span for CreatureService.getAllCreatures");
            span->setAttribute("endpoint", "getAllCreatures");
            span->setAttribute("ws_service", "CreatureService");
        }

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->getAllCreatures(creatures::SortBy::name, true, span); // Pass the span to the database call
        if(!result.isSuccess()) {

            // If we get an error, let's set it up right
            auto errorCode = result.getError().value().getCode();
            switch(errorCode) {
                case ServerError::NotFound:
                    status = Status::CODE_404;
                    break;
                case ServerError::InvalidData:
                    status = Status::CODE_400;
                    break;
                default:
                    status = Status::CODE_500;
                    break;
            }
            errorMessage = result.getError()->getMessage();
            appLogger->warn(std::string(result.getError()->getMessage()));

            // Update the span with the error
            if (span) {
                span->setError(std::string(errorMessage));
                span->setAttribute("error.type", [errorCode]() {
                    switch(errorCode) {
                        case ServerError::NotFound: return "NotFound";
                        case ServerError::InvalidData: return "InvalidData";
                        case ServerError::DatabaseError: return "DatabaseError";
                        default: return "InternalError";
                    }
                }());
                span->setAttribute("error.code", static_cast<int64_t>(errorCode));
            }

            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)


        auto items = oatpp::Vector<oatpp::Object<creatures::CreatureDto>>::createShared();

        auto creatures = result.getValue().value();
        for (const auto &creature : creatures) {
            appLogger->debug("Adding creature: {}", creature.id);
            items->emplace_back(creatures::convertToDto(creature));
        }

        auto page = ListDto<oatpp::Object<creatures::CreatureDto>>::createShared();
        page->count = items->size();
        page->items = items;

        // Record success metrics in the span
        if (span) {
            span->setAttribute("creatures.count", static_cast<int64_t>(page->count));
            span->setSuccess();
        }

        return page;

    }

    oatpp::Object<creatures::CreatureDto> CreatureService::getCreature(const oatpp::String& inCreatureId, std::shared_ptr<RequestSpan> parentSpan) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        // Convert the oatpp string to a std::string
        creatureId_t creatureId = std::string(inCreatureId);

        appLogger->debug("CreatureService::getCreature({})", creatureId);

        auto span = creatures::observability->createOperationSpan("CreatureService.getCreature", parentSpan);

        if (span) {
            span->setAttribute("service", "CreatureService");
            span->setAttribute("operation", "getCreature");
            span->setAttribute("creature.id", std::string(creatureId));
        }

        debug("get creature by ID via REST API: {}", std::string(creatureId));

        if (span) {
            span->setAttribute("creature.id", std::string(creatureId));
        }

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->getCreature(creatureId, span); // Pass the span to the database call
        if(!result.isSuccess()) {

            // If we get an error, let's set it up right
            auto errorCode = result.getError().value().getCode();
            switch(errorCode) {
                case ServerError::NotFound:
                    status = Status::CODE_404;
                    break;
                case ServerError::InvalidData:
                    status = Status::CODE_400;
                    break;
                default:
                    status = Status::CODE_500;
                    break;
            }
            errorMessage = result.getError()->getMessage();
            appLogger->warn(std::string(result.getError()->getMessage()));

            if (span) {
                span->setError(std::string(errorMessage));
                span->setAttribute("error.type", [errorCode]() {
                    switch(errorCode) {
                        case ServerError::NotFound: return "NotFound";
                        case ServerError::InvalidData: return "InvalidData";
                        case ServerError::DatabaseError: return "DatabaseError";
                        default: return "InternalError";
                    }
                }());
                span->setAttribute("error.code", static_cast<int64_t>(errorCode));
            }

            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)

        auto creature = result.getValue().value();

        if (span) {
            span->setAttribute("creature.name", creature.name);
            span->setSuccess();
        }

        return creatures::convertToDto(creature);

    }


    oatpp::Object<creatures::CreatureDto> CreatureService::upsertCreature(const std::string& jsonCreature, std::shared_ptr<RequestSpan> parentSpan) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        auto serviceSpan = creatures::observability->createOperationSpan("CreatureService.upsertCreature", parentSpan);

        appLogger->info("attempting to upsert a creature");

        if (serviceSpan) {
            serviceSpan->setAttribute("service", "CreatureService");
            serviceSpan->setAttribute("operation", "upsertCreature");
            serviceSpan->setAttribute("json.size", static_cast<int64_t>(jsonCreature.length()));
        }
        appLogger->trace("JSON: {}", jsonCreature);

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        // ✨ Create a span for the validation step in the service
        auto validationSpan = creatures::observability->createChildOperationSpan("CreatureService.validateJson", serviceSpan);

        // Validate the JSON
        try {

            /*
             * This is a bit weird. Yes we're parsing the JSON twice. Once here, and once in upsertCreature().
             * This is because we need to validate the JSON before we pass it off to the database. If we don't,
             * we'll get a cryptic error from the database that doesn't tell us what's wrong with the JSON.
             *
             * We have to do this twice because the database stores whatever the client gives us. This means
             * that we need to pass in the raw JSON, but we also need to validate it here.
             */
            auto jsonObject = nlohmann::json::parse(jsonCreature);
            auto result = db->validateCreatureJson(jsonObject);
            if (validationSpan) validationSpan->setAttribute("validator", "validateCreatureJson");

            if(!result.isSuccess()) {
                errorMessage = result.getError()->getMessage();
                appLogger->warn(std::string(result.getError()->getMessage()));
                status = Status::CODE_400;
                if(validationSpan) validationSpan->setError(std::string(errorMessage));
                if(serviceSpan) serviceSpan->setError(std::string(errorMessage));
                error = true;
            }
        }
        catch ( const nlohmann::json::parse_error& e) {
            errorMessage = e.what();
            appLogger->warn(std::string(e.what()));
            status = Status::CODE_400;
            if(validationSpan) validationSpan->recordException(e);
            if(serviceSpan) serviceSpan->recordException(e);
            error = true;
        }
        if(validationSpan && !error) validationSpan->setSuccess();
        OATPP_ASSERT_HTTP(!error, status, errorMessage)


        appLogger->debug("passing the upsert request off to the database");
        auto result = db->upsertCreature(jsonCreature, serviceSpan);

        // If there's an error, let the client know
        if(!result.isSuccess()) {

            errorMessage = result.getError()->getMessage();
            appLogger->warn(std::string(result.getError()->getMessage()));
            status = Status::CODE_500;
            if(serviceSpan) serviceSpan->setError(std::string(errorMessage));
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)

        // This should never happen and is a bad bug if it does 😱
        if(!result.getValue().has_value()) {
            errorMessage = "DB didn't return a value after upserting the creature. This is a bug. Please report it.";
            appLogger->error(std::string(errorMessage));
            if(serviceSpan) serviceSpan->setError(std::string(errorMessage));
            OATPP_ASSERT_HTTP(true, Status::CODE_500, errorMessage);
        }

        // Yay! All good! Send it along
        auto creature = result.getValue().value();
        info("Updated {} in the database", creature.name);
        if(serviceSpan) {
            serviceSpan->setAttribute("creature.id", creature.id);
            serviceSpan->setAttribute("creature.name", creature.name);
            serviceSpan->setSuccess();
        }
        return convertToDto(creature);

    }


} // creatures :: ws
