

#include <string>
#include <vector>

#include "model/Playlist.h"
#include "server/database.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

#include "PlaylistService.h"


namespace creatures {
    extern std::shared_ptr<Database> db;
}

namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;

    oatpp::Object<ListDto<oatpp::Object<creatures::PlaylistDto>>> PlaylistService::getAllPlaylists() {

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->debug("PlaylistService::getAllPlaylists()");

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->getAllPlaylists();
        if(!result.isSuccess()) {

            debug("getAllPlaylists() was not a success 😫");

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
        appLogger->trace("done fetching items");


        auto items = oatpp::Vector<oatpp::Object<creatures::PlaylistDto>>::createShared();

        auto playlists = result.getValue().value();
        for (const auto &playlist : playlists) {
            appLogger->debug("Adding playlist: {}", playlist.id);
            items->emplace_back(creatures::convertToDto(playlist));
            appLogger->trace("added");
        }

        auto page = ListDto<oatpp::Object<creatures::PlaylistDto>>::createShared();
        page->count = items->size();
        page->items = items;

        return page;

    }


    oatpp::Object<creatures::PlaylistDto> PlaylistService::getPlaylist(const oatpp::String &inPlaylistId) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        // Convert the oatpp string to a std::string
        std::string playlistId = std::string(inPlaylistId);

        appLogger->debug("PlaylistService::getPlaylist({})", playlistId);

        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        auto result = db->getPlaylist(playlistId);
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

        auto playlist = result.getValue().value();
        return creatures::convertToDto(playlist);

    }

    oatpp::Object<creatures::PlaylistDto> PlaylistService::upsertPlaylist(const std::string &playlistJson) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);


        appLogger->info("attempting to upsert a playlist");

        appLogger->debug("JSON: {}", playlistJson);


        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;

        try {

            /*
             * There's the same weirdness here that's in the Creature version of this Service (which is what
             * this one is based on). I want to be able to store the raw JSON in the database, but I also want
             * to validate it to make sure it has what data the front end needs.
             */
            auto jsonObject = nlohmann::json::parse(playlistJson);
            auto result = db->validatePlaylistJson(jsonObject);
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
        auto result = db->upsertPlaylist(playlistJson);

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
            errorMessage = "DB didn't return a value after upserting a playlist. This is a bug. Please report it.";
            appLogger->error(std::string(errorMessage));
            OATPP_ASSERT_HTTP(true, Status::CODE_500, errorMessage);
        }

        // Yay! All good! Send it along
        auto playlist = result.getValue().value();
        info("Updated playlist '{}' in the database (id: {})",
             playlist.name, playlist.id);
        return convertToDto(playlist);
    }

//
//    oatpp::Object<creatures::ws::StatusDto> AnimationService::playStoredAnimation(const oatpp::String& animationId, universe_t universe) {
//        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
//
//        appLogger->debug("AnimationService::playStoredAnimation({}, {})", std::string(animationId), universe);
//
//        bool error = false;
//        oatpp::String errorMessage;
//        Status status = Status::CODE_200;
//
//        auto result = db->playStoredAnimation(std::string(animationId), universe);
//        if(!result.isSuccess()) {
//
//            auto errorCode = result.getError().value().getCode();
//            switch(errorCode) {
//                case ServerError::NotFound:
//                    status = Status::CODE_404;
//                    break;
//                case ServerError::InvalidData:
//                    status = Status::CODE_400;
//                    break;
//                default:
//                    status = Status::CODE_500;
//                    break;
//            }
//            errorMessage = result.getError().value().getMessage();
//            appLogger->warn(std::string(errorMessage));
//            error = true;
//        }
//        OATPP_ASSERT_HTTP(!error, status, errorMessage)
//
//        auto playResult = StatusDto::createShared();
//        playResult->status = "OK";
//        playResult->message = result.getValue().value();
//        playResult->code = 200;
//
//        return playResult;
//    }

} // creatures :: ws