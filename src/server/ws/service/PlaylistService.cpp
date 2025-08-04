

#include <string>

#include "model/Playlist.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "server/ws/dto/ListDto.h"
#include "util/JsonParser.h"
#include "util/cache.h"

#include "PlaylistService.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObjectCache<universe_t, PlaylistStatus>> runningPlaylists;
} // namespace creatures

namespace creatures ::ws {

using oatpp::web::protocol::http::Status;

oatpp::Object<ListDto<oatpp::Object<creatures::PlaylistDto>>> PlaylistService::getAllPlaylists() {

    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    appLogger->debug("PlaylistService::getAllPlaylists()");

    bool error = false;
    std::string errorMessage;
    Status status = Status::CODE_200;

    auto result = db->getAllPlaylists();
    if (!result.isSuccess()) {

        debug("getAllPlaylists() was not a success 😫");

        uint16_t errorValue;

        // If we get an error, let's set it up right
        auto errorCode = result.getError().value().getCode();
        switch (errorCode) {
        case ServerError::NotFound:
            status = Status::CODE_404;
            errorValue = 404;
            break;
        case ServerError::InvalidData:
            status = Status::CODE_400;
            errorValue = 400;
            break;
        default:
            status = Status::CODE_500;
            errorValue = 500;
            break;
        }
        errorMessage = fmt::format("getAllPlaylists() error {}: {}", errorValue, result.getError()->getMessage());
        appLogger->warn(errorMessage);
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)
    appLogger->trace("done fetching items");

    auto items = oatpp::Vector<oatpp::Object<creatures::PlaylistDto>>::createShared();
    auto playlists = result.getValue().value();

    // If there aren't any playlists, return a 404
    if (playlists.empty()) {
        errorMessage = "No playlists found";
        appLogger->warn(errorMessage);
        OATPP_ASSERT_HTTP(false, Status::CODE_404, errorMessage)
    }

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
    if (!result.isSuccess()) {

        auto errorCode = result.getError().value().getCode();
        switch (errorCode) {
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
        auto jsonResult = JsonParser::parseJsonString(playlistJson, "playlist upsert validation");
        if (!jsonResult.isSuccess()) {
            auto parseError = jsonResult.getError().value();
            errorMessage = parseError.getMessage();
            appLogger->warn(std::string(errorMessage));
            status = Status::CODE_400;
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)
        auto jsonObject = jsonResult.getValue().value();
        auto result = db->validatePlaylistJson(jsonObject);
        if (!result.isSuccess()) {
            errorMessage = result.getError()->getMessage();
            appLogger->warn(std::string(result.getError()->getMessage()));
            status = Status::CODE_400;
            error = true;
        }
    } catch (const nlohmann::json::parse_error &e) {
        errorMessage = e.what();
        appLogger->warn(std::string(e.what()));
        status = Status::CODE_400;
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    appLogger->debug("passing the upsert request off to the database");
    auto result = db->upsertPlaylist(playlistJson);

    // If there's an error, let the client know
    if (!result.isSuccess()) {

        errorMessage = result.getError()->getMessage();
        appLogger->warn(std::string(result.getError()->getMessage()));
        status = Status::CODE_500;
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    // This should never happen and is a bad bug if it does 😱
    if (!result.getValue().has_value()) {
        errorMessage = "DB didn't return a value after upserting a playlist. This is a bug. Please report it.";
        appLogger->error(std::string(errorMessage));
        OATPP_ASSERT_HTTP(true, Status::CODE_500, errorMessage);
    }

    // Yay! All good! Send it along
    auto playlist = result.getValue().value();
    info("Updated playlist '{}' in the database (id: {})", playlist.name, playlist.id);
    return convertToDto(playlist);
}

oatpp::Object<creatures::ws::StatusDto> PlaylistService::startPlaylist(universe_t universe,
                                                                       const oatpp::String &inPlaylistId) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    // Convert the oatpp string to a std::string
    std::string playlistId = std::string(inPlaylistId);

    appLogger->debug("PlaylistService::startPlaylist({}, {})", universe, std::string(playlistId));

    bool error = false;
    oatpp::String errorMessage;
    Status status = Status::CODE_200;

    // First make sure the playlist is valid
    if (playlistId.empty()) {
        errorMessage = "No playlist ID provided";
        appLogger->warn(std::string(errorMessage));
        status = Status::CODE_400;
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    // Now go see if that playlist exists
    auto playlistResult = db->getPlaylist(playlistId);
    if (!playlistResult.isSuccess()) {
        auto errorCode = playlistResult.getError().value().getCode();
        switch (errorCode) {
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
        errorMessage = playlistResult.getError().value().getMessage();
        appLogger->warn(std::string(errorMessage));
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    // Yay it exists!
    auto playlist = playlistResult.getValue().value();
    debug("confirmed that playlist '{}' exists ({})", playlist.id, playlist.name);

    // Now let's go start it

    // Set the playlist in the cache
    PlaylistStatus playlistStatus;
    playlistStatus.universe = universe;
    playlistStatus.playlist = playlistId;
    playlistStatus.playing = true;
    playlistStatus.current_animation = ""; // Will get filled in on the next frame
    runningPlaylists->put(universe, playlistStatus);

    auto playEvent = std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), universe);
    eventLoop->scheduleEvent(playEvent);

    std::string okayMessage = fmt::format("🎵 Started playing playlist {} on universe {}", playlist.name, universe);
    info(okayMessage);

    metrics->incrementPlaylistsStarted();

    auto response = StatusDto::createShared();
    response->code = 200;
    response->message = "Started playback";
    response->status = "OK";

    debug("returning a 200");
    return response;
}

oatpp::Object<creatures::ws::StatusDto> PlaylistService::stopPlaylist(universe_t universe) {

    info("stopping playlist on universe {}", universe);

    // Remove the playlist from the cache
    runningPlaylists->remove(universe);

    auto response = StatusDto::createShared();
    response->code = 200;
    response->message = "Stopped playback";
    response->status = "OK";

    debug("returning a 200");
    return response;
}

oatpp::Object<creatures::PlaylistStatusDto> PlaylistService::playlistStatus(universe_t universe) {
    debug("returning the status of the playlist on universe {}", universe);

    if (runningPlaylists->contains(universe)) {
        auto playlistStatus = runningPlaylists->get(universe);
        return convertToDto(*playlistStatus);

    } else {

        // It doesn't exist, so let's make a blank one
        PlaylistStatus playlistStatus;
        playlistStatus.universe = universe;
        playlistStatus.playlist = "";
        playlistStatus.playing = false;
        playlistStatus.current_animation = "";
        return convertToDto(playlistStatus);
    }
}

oatpp::Object<ListDto<oatpp::Object<creatures::PlaylistStatusDto>>> PlaylistService::getAllPlaylistStatuses() {
    debug("returning the status of all playlists");

    auto playlists = oatpp::Vector<oatpp::Object<creatures::PlaylistStatusDto>>::createShared();

    auto keys = runningPlaylists->getAllKeys();
    for (const auto &key : keys) {
        playlists->emplace_back(playlistStatus(key));
    }

    auto page = ListDto<oatpp::Object<creatures::PlaylistStatusDto>>::createShared();
    page->count = playlists->size();
    page->items = playlists;
    return page;
}

} // namespace creatures::ws