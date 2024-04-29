


#include "exception/exception.h"
#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/database.h"

#include "server/ws/dto/PageDto.h"
#include "server/ws/dto/StatusDto.h"

#include "AnimationService.h"


namespace creatures {
    extern std::shared_ptr<Database> db;
}


namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;

    oatpp::Object<PageDto<oatpp::Object<creatures::AnimationMetadataDto>>> AnimationService::listAllAnimations() {

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->info("AnimationService::listAllAnimations()");

        bool error = false;
        oatpp::String errorMessage;
        std::vector<creatures::AnimationMetadata> metadatas;
        Status status = Status::CODE_200;

        try {
            metadatas = creatures::db->listAnimations(creatures::SortBy::name);
            appLogger->debug("Found {} animations", metadatas.size());
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
        catch (const creatures::NotFoundException &e) {
            errorMessage = fmt::format("No animations found: {}", e.what());
            appLogger->error(std::string(errorMessage));
            error = true;
            status = Status::CODE_404;
        }
        catch (...) {
            errorMessage = fmt::format("Unknown error");
            appLogger->error(std::string(errorMessage));
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)



        auto items = oatpp::Vector<oatpp::Object<creatures::AnimationMetadataDto>>::createShared();

        for (const auto &metadata : metadatas) {
            appLogger->debug("Adding animation: {}", metadata.animation_id);
            items->emplace_back(creatures::convertToDto(metadata));
        }

        auto page = PageDto<oatpp::Object<creatures::AnimationMetadataDto>>::createShared();
        page->count = items->size();
        page->items = items;

        return page;

    }


    oatpp::Object<creatures::AnimationDto> AnimationService::getAnimation(const oatpp::String &inAnimationId) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        // Convert the oatpp string to a std::string
        std::string animationId = std::string(inAnimationId);

        appLogger->info("AnimationService::getAnimation({})", animationId);

        bool error = false;
        oatpp::String errorMessage;
        creatures::Animation animation;
        Status status = Status::CODE_200;

        try {
            animation = creatures::db->getAnimation(animationId);
            appLogger->debug("Found animation: {}", animation.metadata.title);
        }
        catch (const creatures::NotFoundException &e) {
            errorMessage = fmt::format("Unable to locate animation {}: {}", animationId, e.what());
            appLogger->debug(std::string(errorMessage));
            error = true;
            status = Status::CODE_404;
        }
        catch (const creatures::InvalidArgumentException &e) {
            errorMessage = fmt::format("Unable to parse animation ID {}: {}", animationId, e.what());
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
        return creatures::convertToDto(animation);

    }


} // creatures :: ws