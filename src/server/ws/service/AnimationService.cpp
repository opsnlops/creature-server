

#include <string>

#include "exception/exception.h"
#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/database.h"

#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

#include "AnimationService.h"


namespace creatures {
    extern std::shared_ptr<Database> db;
}


namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;

    oatpp::Object<ListDto<oatpp::Object<creatures::AnimationMetadataDto>>> AnimationService::listAllAnimations() {

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->debug("AnimationService::listAllAnimations()");

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->listAnimations(creatures::SortBy::name);
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


        auto items = oatpp::Vector<oatpp::Object<creatures::AnimationMetadataDto>>::createShared();

        auto metadatas = result.getValue().value();
        for (const auto &metadata : metadatas) {
            appLogger->debug("Adding animation metadata: {}", metadata.animation_id);
            items->emplace_back(creatures::convertToDto(metadata));
        }

        auto page = ListDto<oatpp::Object<creatures::AnimationMetadataDto>>::createShared();
        page->count = items->size();
        page->items = items;

        return page;

    }


    oatpp::Object<creatures::AnimationDto> AnimationService::getAnimation(const oatpp::String &inAnimationId) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        // Convert the oatpp string to a std::string
        std::string animationId = std::string(inAnimationId);

        appLogger->debug("AnimationService::getAnimation({})", animationId);

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->getAnimation(animationId);
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
            errorMessage = result.getError().value().getMessage();
            appLogger->warn(std::string(errorMessage));
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)

        auto animation = result.getValue().value();
        return creatures::convertToDto(animation);

    }

    oatpp::Object<creatures::AnimationDto> AnimationService::upsertAnimation(const std::string& jsonAnimation) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);


        appLogger->info("attempting to upsert an animation");

        appLogger->trace("JSON: {}", jsonAnimation);


        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        try {

            /*
             * There's the same weirdness here that's in the Creature version of this Service (which is what
             * this one is based on). I want to be able to store the raw JSON in the database, but I also want
             * to validate it to make sure it has what data the front end needs.
             */
            auto jsonObject = nlohmann::json::parse(jsonAnimation);
            auto result = db->validateAnimationJson(jsonObject);
            if(!result.isSuccess()) {
                errorMessage = result.getError()->getMessage();
                appLogger->warn(std::string(result.getError()->getMessage()));
                status = Status::CODE_400;
                error = true;
            }
        }
        catch ( const nlohmann::json::parse_error& e) {
            errorMessage = e.what();
            appLogger->warn(std::string(e.what()));
            status = Status::CODE_400;
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)




        appLogger->debug("passing the upsert request off to the database");
        auto result = db->upsertAnimation(jsonAnimation);

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
            errorMessage = "DB didn't return a value after upserting an animation. This is a bug. Please report it.";
            appLogger->error(std::string(errorMessage));
            OATPP_ASSERT_HTTP(true, Status::CODE_500, errorMessage);
        }

        // Yay! All good! Send it along
        auto animation = result.getValue().value();
        info("Updated animation '{}' in the database (id: {})",
             animation.metadata.title, animation.id);
        return convertToDto(animation);
    }

} // creatures :: ws