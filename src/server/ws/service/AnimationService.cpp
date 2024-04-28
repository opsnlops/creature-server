


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

        try {
            metadatas = creatures::db->listAnimations(creatures::SortBy::name);
            appLogger->debug("Found {} animations", metadatas.size());
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


} // creatures :: ws