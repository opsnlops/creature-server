
#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

// Disable shadow warnings for MongoDB C++ driver headers (third-party code)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"

#include <bsoncxx/array/element.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>

#pragma GCC diagnostic pop

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Creature.h"
#include "model/Playlist.h"
#include "model/SortBy.h"
#include "model/Track.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures {

struct AdHocAnimationRecord {
    creatures::Animation animation;
    std::chrono::system_clock::time_point createdAt;
};

class Database {

  public:
    explicit Database(const std::string &mongoURI_);

    // Creature stuff
    Result<creatures::Creature> getCreature(const creatureId_t &creatureId,
                                            const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<std::vector<creatures::Creature>>
    getAllCreatures(creatures::SortBy sortBy, bool ascending,
                    const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<json> getCreatureJson(creatureId_t creatureId, const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /**
     * Upsert a creature in the database
     *
     * @param creatureJson The full JSON string of the creature. It's stored in the database (as long as all of the
     *                     needed fields are there) so that the controller and console get get a full view of what
     *                     the creature actually is.
     *
     * @return a `Result<creatures::Creature>` with the encoded creature that we can return to the client
     */
    Result<creatures::Creature> upsertCreature(const std::string &creatureJson,
                                               const std::shared_ptr<OperationSpan> &parentSpan = nullptr);

    /**
     * Validates that the JSON for a Creature contains the fields we expect.
     *
     * It only validates `inputs` if it exists!
     *
     * @param json the JSON to validate
     * @return true if good, or ServerError if not
     */
    static Result<bool> validateCreatureJson(const nlohmann::json &json);

    /**
     * Validates that the JSON for an Animation contains the fields we expect.
     *
     * @param json the JSON to validate
     * @return true if good, or ServerError if not
     */
    static Result<bool> validateAnimationJson(const nlohmann::json &json);

    /**
     * Validates that the JSON for an Playlist contains the fields we expect.
     *
     * @param json the JSON to validate
     * @return true if good, or ServerError if not
     */
    static Result<bool> validatePlaylistJson(const nlohmann::json &json);

    /**
     * Helper function that checks if a JSON object has all of the required fields. Used
     * heavily by `validate[Thing]Json()`.
     *
     * @param j the JSON object to check
     * @param required_fields an array of fields to validate
     * @return true if all fields are present, or a ServerError if not
     */
    static Result<bool> has_required_fields(const nlohmann::json &j, const std::vector<std::string> &required_fields);

    /*
     * Since the format of the Animations changed completely in Animations 2.0, I'm just removing
     * the old gRPC-based stuff. It's not worth trying to port it over.
     */

    // Animation stuff
    Result<json> getAnimationJson(animationId_t animationId, std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<creatures::Animation> getAnimation(const animationId_t &animationId,
                                              std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<std::vector<creatures::AnimationMetadata>>
    listAnimations(creatures::SortBy sortBy, const std::shared_ptr<OperationSpan> &parentSpan = nullptr);
    Result<creatures::Animation> upsertAnimation(const std::string &animationJson,
                                                 std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<std::string> playStoredAnimation(animationId_t animationId, universe_t universe,
                                            std::shared_ptr<OperationSpan> parentSpan = nullptr);

    // Playlist stuff
    Result<json> getPlaylistJson(playlistId_t playlistId, std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<std::vector<creatures::Playlist>> getAllPlaylists(std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<creatures::Playlist> getPlaylist(const playlistId_t &playlistId,
                                            std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<creatures::Playlist> upsertPlaylist(const std::string &playlistJson,
                                               std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Ensure supporting indexes (including TTL) for the ad-hoc animation collection exist.
     */
    Result<void> ensureAdHocAnimationIndexes(uint32_t ttlHours);

    /**
     * Insert a freshly generated ad-hoc animation into the TTL collection.
     */
    Result<void> insertAdHocAnimation(const creatures::Animation &animation,
                                      std::chrono::system_clock::time_point createdAt,
                                      std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<std::vector<AdHocAnimationRecord>>
    listAdHocAnimations(std::shared_ptr<OperationSpan> parentSpan = nullptr);
    Result<creatures::Animation> getAdHocAnimation(const animationId_t &animationId,
                                                   std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Request that the database perform a health check
     *
     * This is what updates the serverPingable flag
     */
    void performHealthCheck();

    /**
     * Can the server be pinged?
     *
     * @return true if the server is pingable
     */
    bool isServerPingable() const;

  protected:
    /**
     * Check to see if a field is present in a JSON object
     *
     * @param jsonObj the object to check
     * @param fieldName the field to look for
     * @return true if it's there, or a ServerError if it's not
     */
    static Result<bool> checkJsonField(const nlohmann::json &jsonObj, const std::string &fieldName);

  private:
    std::string mongoURI;
    mongocxx::pool mongoPool;

    Result<mongocxx::collection> getCollection(const std::string &collectionName);

    static Result<creatures::Creature> creatureFromJson(json creatureJson,
                                                        std::shared_ptr<OperationSpan> parentSpan = nullptr);

    static Result<creatures::Animation> animationFromJson(json animationJson);
    static Result<creatures::AnimationMetadata> animationMetadataFromJson(json animationMetadataJson);
    static Result<creatures::Track> trackFromJson(json trackJson);

    /*
     * Playlists
     */
    static Result<creatures::Playlist> playlistFromJson(json playlistJson,
                                                        std::shared_ptr<OperationSpan> parentSpan = nullptr);
    static Result<creatures::PlaylistItem> playlistItemFromJson(json playlistItemJson,
                                                                std::shared_ptr<OperationSpan> parentSpan = nullptr);

    // Start out thinking that the server is pingable
    std::atomic<bool> serverPingable{true};
};

} // namespace creatures
