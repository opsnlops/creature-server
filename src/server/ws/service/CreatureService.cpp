

#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "util/Result.h"
#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"


#include "server/ws/dto/ListDto.h"

#include "CreatureService.h"


namespace creatures {
    extern std::shared_ptr<Database> db;
}


namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;

    oatpp::Object<ListDto<oatpp::Object<creatures::CreatureDto>>> CreatureService::getAllCreatures() {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->debug("CreatureService::getAllCreatures()");

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->getAllCreatures(creatures::SortBy::name, true);
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

        return page;

    }

    oatpp::Object<creatures::CreatureDto> CreatureService::getCreature(const oatpp::String& inCreatureId) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        // Convert the oatpp string to a std::string
        creatureId_t creatureId = std::string(inCreatureId);

        appLogger->debug("CreatureService::getCreature({})", creatureId);

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->getCreature(creatureId);
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
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)

        auto creature = result.getValue().value();
        return creatures::convertToDto(creature);

    }


    oatpp::Object<creatures::CreatureDto> CreatureService::upsertCreature(const std::string& jsonCreature) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->info("attempting to upsert a creature");

        appLogger->trace("JSON: {}", jsonCreature);

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        appLogger->debug("passing the upsert request off to the database");
        auto result = db->upsertCreature(jsonCreature);

        // If there's an error, let the client know
        if(!result.isSuccess()) {

            errorMessage = result.getError()->getMessage();
            appLogger->warn(std::string(result.getError()->getMessage()));
            status = Status::CODE_500;
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)

        // This should never happen and is a bad bug if it does 😱
        if(!result.getValue().has_value()) {
            errorMessage = "DB didn't return a value after upserting the creature. This is a bug. Please report it.";
            appLogger->error(std::string(errorMessage));
            OATPP_ASSERT_HTTP(true, Status::CODE_500, errorMessage);
        }

        // Yay! All good! Send it along
        auto creature = result.getValue().value();
        info("Updated {} in the database", creature.name);
        return convertToDto(creature);

    }


} // creatures :: ws