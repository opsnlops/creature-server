
#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/pool.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/array/element.hpp>
#include <mongocxx/cursor.hpp>

#include <bsoncxx/builder/stream/document.hpp>

#include <nlohmann/json.hpp>
using json = nlohmann::json;


#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Creature.h"
#include "model/FrameData.h"
#include "model/SortBy.h"
#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

    class Database {

    public:
        explicit Database(mongocxx::pool &pool);

        // Creature stuff


        Result<creatures::Creature> getCreature(const creatureId_t& creatureId);
        std::vector<creatures::Creature> getAllCreatures(creatures::SortBy sortBy, bool ascending);




        /**
         * Upsert a creature in the database
         *
         * @param creatureJson The full JSON string of the creature. It's stored in the database (as long as all of the
         *                     needed fields are there) so that the controller and console get get a full view of what
         *                     the creature actually is.
         *
         * @return a `Result<creatures::Creature>` with the encoded creature that we can return to the client
         */
        Result<creatures::Creature> upsertCreature(const std::string& creatureJson);




        /*
         * Since the format of the Animations changed completely in Animations 2.0, I'm just removing
         * the old gRPC-based stuff. It's not worth trying to port it over.
         */

        // Animation stuff
        creatures::Animation getAnimation(const std::string& animationId);
        std::vector<creatures::AnimationMetadata> listAnimations(creatures::SortBy sortBy);
        std::string createAnimation(creatures::Animation animation);

        // Old gRPC methods
//        void createAnimation(const Animation *animation, DatabaseInfo *reply);
//        void listAnimations(const AnimationFilter *filter, ListAnimationsResponse *animationList);
//        void getAnimation(const AnimationId *animationId, Animation *animation);
//        void getAnimationMetadata(const AnimationId *animationId, AnimationMetadata *animationMetadata);
//        void updateAnimation(const Animation *animation);


        // Playlist stuff
//        grpc::Status createPlaylist(const Playlist *playlist, DatabaseInfo *reply);
//        grpc::Status listPlaylists(const PlaylistFilter *filter, ListPlaylistsResponse *animationList);
//        void getPlaylist(const PlaylistIdentifier *playlistIdentifier, Playlist *playlist);
//        void updatePlaylist(const Playlist *playlist);


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
        bool isServerPingable();


    protected:
        /**
         * Check to see if a field is present in a JSON object
         *
         * @param jsonObj the object to check
         * @param fieldName the field to look for
         * @return true if it's there, or a ServerError if it's not
         */
        static Result<bool> checkJsonField(const nlohmann::json& jsonObj, const std::string& fieldName);

    private:
        mongocxx::pool &pool;

        mongocxx::collection getCollection(const std::string &collectionName);

        bsoncxx::document::value creatureToBson(const creatures::Creature &creature);
        //static bsoncxx::document::value gRPCcreatureToBson(const server::Creature *creature, bool assignNewId);

        creatures::Creature creatureFromBson(const bsoncxx::document::view &doc);
        //static void gRPCCreatureFromBson(const bsoncxx::document::view &doc, server::Creature *creature);

        //static void creatureIdentifierFromBson(const bsoncxx::document::view &doc, CreatureIdentifier *identifier);


        Result<json> getCreatureJson(creatureId_t creatureId);
        static Result<creatures::Creature> creatureFromJson(json creatureJson);



        /*
         * Playlists
         */
//        static bsoncxx::document::value playlistToBson(const Playlist *playlist, bsoncxx::oid playlistId);
//        static int32_t playlistItemsToBson(bsoncxx::builder::stream::document &doc, const server::Playlist *playlist);
//        static void bsonToPlaylist(const bsoncxx::document::view &doc, Playlist *playlist);
//        static void bsonToPlaylistItems(const bsoncxx::document::view &doc, server::Playlist *playlist);

        // Start out thinking that the server is pingable
        std::atomic<bool> serverPingable{true};


    };



}
