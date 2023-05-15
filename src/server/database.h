
#pragma once

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
#include "messaging/server.pb.h"

#include <google/protobuf/timestamp.pb.h>


using server::Animation;
using server::Animation_Metadata;
using server::AnimationFilter;
using server::AnimationId;
using server::Creature;
using server::CreatureFilter;
using server::CreatureName;
using server::CreatureId;
using server::CreatureIdentifier;
using server::DatabaseInfo;
using server::GetAllCreaturesResponse;
using server::ListAnimationsResponse;
using server::ListCreaturesResponse;


namespace creatures {

    class Database {

    public:
        explicit Database(mongocxx::pool &pool);

        grpc::Status createCreature(const Creature *creature, DatabaseInfo *reply);

        grpc::Status updateCreature(const Creature *creature, DatabaseInfo *reply);

        grpc::Status searchCreatures(const CreatureName *creatureName, Creature *creature);

        void getCreature(const CreatureId *creatureId, Creature *creature);

        grpc::Status getAllCreatures(const CreatureFilter *filter, GetAllCreaturesResponse *creatureList);

        grpc::Status listCreatures(const CreatureFilter *filter, ListCreaturesResponse *creatureList);

        grpc::Status createAnimation(const Animation *animation, DatabaseInfo *reply);

        grpc::Status listAnimations(const AnimationFilter *filter, ListAnimationsResponse *animationList);

        void getAnimation(const AnimationId *animationId, Animation *animation);

        // This throws a lot of errors :)
        void updateAnimation(const Animation *animation);

        /**
         * Ping the database to make sure it's alive
         *
         * @return Status::OK if the DB is okay
         */
        grpc::Status ping();

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
        static void bsonToAnimationMetadata(const bsoncxx::document::view &doc, Animation_Metadata *metadata);
        static void populateFramesFromBson(const bsoncxx::document::view &doc, Animation *animation);
    };



}
