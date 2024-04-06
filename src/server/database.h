
#pragma once

#include <atomic>
#include <chrono>
#include <string>

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

#include <grpcpp/grpcpp.h>
#include "server.pb.h"

#include <google/protobuf/timestamp.pb.h>

#include "server/namespace-stuffs.h"

namespace creatures {

    class Database {

    public:
        explicit Database(mongocxx::pool &pool);

        // Creature stuff
        void createCreature(const Creature *creature, DatabaseInfo *reply);
        void updateCreature(const Creature *creature);
        void searchCreatures(const CreatureName *creatureName, Creature *creature);
        void getCreature(const CreatureId *creatureId, Creature *creature);
        void getAllCreatures(const CreatureFilter *filter, GetAllCreaturesResponse *creatureList);
        void listCreatures(const CreatureFilter *filter, ListCreaturesResponse *creatureList);



        // Animation stuff
        void createAnimation(const Animation *animation, DatabaseInfo *reply);
        void listAnimations(const AnimationFilter *filter, ListAnimationsResponse *animationList);
        void getAnimation(const AnimationId *animationId, Animation *animation);
        void getAnimationIdentifier(const AnimationId *animationId, AnimationIdentifier *animationIdentifier);
        void updateAnimation(const Animation *animation);


        // Playlist stuff
        grpc::Status createPlaylist(const Playlist *playlist, DatabaseInfo *reply);
        grpc::Status listPlaylists(const PlaylistFilter *filter, ListPlaylistsResponse *animationList);
        void getPlaylist(const PlaylistIdentifier *playlistIdentifier, Playlist *playlist);
        void updatePlaylist(const Playlist *playlist);


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

    private:
        mongocxx::pool &pool;

        mongocxx::collection getCollection(const std::string &collectionName);

        static bsoncxx::document::value creatureToBson(const Creature *creature, bool assignNewId);

        static void creatureFromBson(const bsoncxx::document::view &doc, Creature *creature);

        static void
        creatureIdentifierFromBson(const bsoncxx::document::view &doc, CreatureIdentifier *identifier);

        static std::chrono::system_clock::time_point
        protobufTimestampToTimePoint(const google::protobuf::Timestamp &timestamp);

        static google::protobuf::Timestamp
        convertMongoDateToProtobufTimestamp(const bsoncxx::document::element &mongo_timestamp_element);

        /**
         * Serialize an animation to BSON
         *
         * @param animation the Animation itself
         * @param animationId the ID of this animation. Passed separately in case we need to assign a new one.
         *                    Done this way since animation is const.
         * @return the BSON document
         */
        static bsoncxx::document::value animationToBson(const Animation *animation, bsoncxx::oid animationId);
        static bsoncxx::document::value metadataToBson(const Animation *animation, bsoncxx::oid animationId);
        static uint32_t framesToBson(bsoncxx::builder::stream::document &doc, const Animation *animation);
        static void bsonToAnimationMetaData(const bsoncxx::document::view &doc, AnimationMetaData *metadata);
        static void populateFramesFromBson(const bsoncxx::document::view &doc, Animation *animation);

        /*
         * Playlists
         */
        static bsoncxx::document::value playlistToBson(const Playlist *playlist, bsoncxx::oid playlistId);
        static int32_t playlistItemsToBson(bsoncxx::builder::stream::document &doc, const server::Playlist *playlist);
        static void bsonToPlaylist(const bsoncxx::document::view &doc, Playlist *playlist);
        static void bsonToPlaylistItems(const bsoncxx::document::view &doc, server::Playlist *playlist);

        // Start out thinking that the server is pingable
        std::atomic<bool> serverPingable{true};

    };



}
