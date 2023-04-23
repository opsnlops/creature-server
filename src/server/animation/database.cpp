
#include "spdlog/spdlog.h"

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/pool.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/array/element.hpp>
#include <mongocxx/cursor.hpp>

#include <bsoncxx/builder/stream/document.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/animation/animation.h"

using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;
using spdlog::trace;

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;


/*
message Animation {
message Metadata {
string title = 1;
int32 frames_per_second = 2;
int32 number_of_frames = 3;
CreatureType creature_type = 4;
int32 number_of_motors = 5;
string notes = 6;
}

message Frame {
repeated bytes bytes = 1;
}

bytes _id = 1;
Metadata metadata = 2;
repeated Frame frames = 3;
}
*/

extern creatures::Database *db;

namespace creatures {



    /**
     * Save a new animation in the database
     *
     * @param context the `ServerContext` of this request
     * @param animation an `Animation` to save
     * @param reply a `DatabaseInfo` that will be filled out for the reply
     * @return state of the request
     */
    Status CreatureServerImpl::CreateAnimation(ServerContext *context,
                                               const Animation *animation,
                                               DatabaseInfo *reply) {

        info("Creating a new animation in the database");
        return db->createAnimation(animation, reply);
    }

    /**
     * Create a new animation in the database
     *
     * @param animation the `Animation` to save
     * @param reply Information about the save
     * @return a gRPC Status on how things went
     */
    grpc::Status Database::createAnimation(const Animation *animation, DatabaseInfo *reply) {

        debug("creating a new animation in the database");

        grpc::Status status;

        auto collection = getCollection(ANIMATIONS_COLLECTION);
        trace("collection obtained");

        // Create a BSON doc with this animation
        try {
            auto doc_view = animationToBson(animation);
            trace("doc_value made: {}", bsoncxx::to_json(doc_view));

            collection.insert_one(doc_view.view());
            trace("run_command done");

            info("saved new animation in the database 💃🏽");

            status = grpc::Status::OK;
            reply->set_message("✅ Saved new animation in the database");
        }
        catch (const mongocxx::exception &e) {
            // Was this an attempt to make a duplicate creature?
            if (e.code().value() == 11000) {
                error("attempted to insert a duplicate Animation in the database for id {}", animation->_id());
                status = grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
                reply->set_message("Unable to create new animation");
                reply->set_help(fmt::format("ID {} already exists", animation->_id()));
            } else {
                critical("Error updating database: {}", e.what());
                status = grpc::Status(grpc::StatusCode::UNKNOWN, e.what(), fmt::to_string(e.code().value()));
                reply->set_message(
                        fmt::format("Unable to create Animation in database: {} ({})", e.what(), e.code().value()));
                reply->set_help(e.code().message());
            }
        }
        catch (creatures::DataFormatException &e) {
            error("server refused to save animation: {}", e.what());
            status = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
            reply->set_message(fmt::format("Unable to create new Animation: {}", e.what()));
            reply->set_help("Sorry! 💜");
        }

        return status;
    }


    bsoncxx::document::value Database::animationToBson(const server::Animation *animation) {

        trace("converting an animation to BSON");

        // If the _id is empty, don't go any further
        if(animation->_id().empty()) {
            error("Unable to save animation in database, _id is empty");
            throw DataFormatException("`_id` is empty");
        }

        bsoncxx::builder::stream::document doc{};

        doc << "_id" << bsoncxx::oid(animation->_id());
        trace("_id set");

        auto metadata = bsoncxx::builder::stream::document{}
                << "title" << animation->metadata().title()
                << "frames_per_second"
                << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().frames_per_second())}
                << "number_of_frames"
                << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().number_of_frames())}
                << "creature_type"
                << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().creature_type())}
                << "number_of_motors"
                << bsoncxx::types::b_int32{static_cast<int32_t>(animation->metadata().number_of_motors())}
                << "notes" << animation->metadata().notes()
                << bsoncxx::builder::stream::finalize;
        doc << "metadata" << metadata;
        trace("metadata created");


        auto frames = doc << "frames" << bsoncxx::builder::stream::open_array;

        trace("starting to add frames");
        uint32_t frameCount = 0;

        for (const auto &f: animation->frames()) {
            const auto *byteData = reinterpret_cast<const uint8_t *>(f.bytes().data());
            size_t byteSize = f.bytes().size();
            std::vector<uint8_t> byteVector(byteData, byteData + byteSize);
            frames << bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                               static_cast<uint32_t>(byteVector.size()), byteVector.data()};
            frameCount++;
        }

        frames << bsoncxx::builder::stream::close_array;
        trace("done adding frames, added {} total", frameCount);

        return doc.extract();
    }

}