

#include <string>

#include "exception/exception.h"
#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/database.h"

#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

#include "util/ObservabilityManager.h"

#include "AnimationService.h"


namespace creatures {
    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObservabilityManager> observability;
}


namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;

    oatpp::Object<ListDto<oatpp::Object<creatures::AnimationMetadataDto>>> AnimationService::listAllAnimations(std::shared_ptr<RequestSpan> parentSpan) {

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        if (!parentSpan) {
            warn("no parent span provided for AnimationService.listAllAnimations, creating a root span");
        }

        // 🐰 Create a trace span for this request
        auto span = creatures::observability->createOperationSpan("AnimationService.listAllAnimations",
            std::move(parentSpan));

        appLogger->debug("AnimationService::listAllAnimations()");

        if (span) {
            trace("adding attributes to the span for AnimationService.listAllAnimations");
            span->setAttribute("endpoint", "listAllAnimations");
            span->setAttribute("ws_service", "AnimationService");
        }


        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->listAnimations(creatures::SortBy::name, span);
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
            span->setError(std::string(errorMessage));

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

        // Record success metrics in the span
        if (span) {
            span->setAttribute("animations.count", static_cast<int64_t>(page->count));
            span->setSuccess();
        }

        return page;

    }


    oatpp::Object<creatures::AnimationDto> AnimationService::getAnimation(const oatpp::String &inAnimationId, std::shared_ptr<RequestSpan> parentSpan) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        // Convert the oatpp string to a std::string
        std::string animationId = std::string(inAnimationId);

        appLogger->debug("AnimationService::getAnimation({})", animationId);

        auto span = creatures::observability->createOperationSpan("AnimationService.getAnimation", parentSpan);

        if (span) {
            span->setAttribute("service", "AnimationService");
            span->setAttribute("operation", "getAnimation");
            span->setAttribute("animation.id", std::string(animationId));
        }

        debug("get animation by ID via REST API: {}", std::string(animationId));

        if (span) {
            span->setAttribute("animation.id", std::string(animationId));
        }

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->getAnimation(animationId, span);
        if(!result.isSuccess()) {

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

        const auto animation = result.getValue().value();

        if (span) {
            span->setAttribute("animation.title", animation.metadata.title);
            span->setAttribute("animation.tracks", static_cast<int64_t>(animation.tracks.size()));
            span->setAttribute("animation.number_of_frames", static_cast<int64_t>(animation.metadata.number_of_frames));
            span->setAttribute("animation.milliseconds_per_frame", static_cast<int64_t>(animation.metadata.milliseconds_per_frame));
            span->setSuccess();
        }

        return creatures::convertToDto(animation);

    }

    oatpp::Object<creatures::AnimationDto> AnimationService::upsertAnimation(const std::string& jsonAnimation) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);


        appLogger->info("attempting to upsert an animation");

        appLogger->debug("JSON: {}", jsonAnimation);


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


    oatpp::Object<creatures::ws::StatusDto> AnimationService::playStoredAnimation(const oatpp::String& animationId, universe_t universe) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->debug("AnimationService::playStoredAnimation({}, {})", std::string(animationId), universe);

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->playStoredAnimation(std::string(animationId), universe);
        if(!result.isSuccess()) {

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

        auto playResult = StatusDto::createShared();
        playResult->status = "OK";
        playResult->message = result.getValue().value();
        playResult->code = 200;

        return playResult;
    }

} // creatures :: ws