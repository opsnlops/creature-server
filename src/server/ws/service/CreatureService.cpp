


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
        std::vector<creatures::Creature> creatures;

        try {
            creatures = creatures::db->getAllCreatures(creatures::SortBy::name, true);
            appLogger->debug("Found {} creatures", creatures.size());
        }
        catch (const creatures::InternalError &e) {
            errorMessage = fmt::format("Internal error: {}", e.what());
            appLogger->error(std::string(errorMessage));
            error = true;
        }
        catch (const creatures::DataFormatException &e) {
            errorMessage = fmt::format("Data format error: {}", e.what());
            appLogger->error(std::string(errorMessage));
            error = true;
        }
        catch (...) {
            errorMessage = fmt::format("Unknown error");
            appLogger->error(std::string(errorMessage));
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, Status::CODE_500, errorMessage)



        auto items = oatpp::Vector<oatpp::Object<creatures::CreatureDto>>::createShared();

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
        std::string creatureId = std::string(inCreatureId);

        appLogger->debug("CreatureService::getCreature({})", creatureId);

        bool error = false;
        oatpp::String errorMessage;
        creatures::Creature creature;
        Status status = Status::CODE_200;

        try {
            creature = creatures::db->getCreature(creatureId);
            appLogger->debug("Found creature: {}", creature.name);
        }
        catch (const creatures::NotFoundException &e) {
            errorMessage = fmt::format("Unable to locate creature {}: {}", creatureId, e.what());
            appLogger->debug(std::string(errorMessage));
            error = true;
            status = Status::CODE_404;
        }
        catch (const creatures::InvalidArgumentException &e) {
            errorMessage = fmt::format("Unable to parse creature ID {}: {}", creatureId, e.what());
            appLogger->debug(std::string(errorMessage));
            error = true;
            status = Status::CODE_400;
        }
        catch (const creatures::InternalError &e) {
            errorMessage = fmt::format("Internal error: {}", e.what());
            appLogger->error(std::string(errorMessage));
            error = true;
            status = Status::CODE_500;
        }
        catch (const creatures::DataFormatException &e) {
            errorMessage = fmt::format("Data format error: {}", e.what());
            appLogger->error(std::string(errorMessage));
            error = true;
            status = Status::CODE_500;
        }
        catch (...) {
            errorMessage = fmt::format("Unknown error");
            appLogger->error(std::string(errorMessage));
            error = true;
            status = Status::CODE_500;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)

        appLogger->debug("returning a 200");
        return creatures::convertToDto(creature);

    }

} // creatures :: ws