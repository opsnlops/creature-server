
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



namespace creatures {

    /**
     * Create a new animation in the database
     *
     * @param animation the `Animation` to save
     * @param reply Information about the save
     * @return a gRPC Status on how things went
     */
    grpc::Status Database::createAnimation(const Animation *animation, DatabaseInfo *reply) {

        debug("creating a new animation in the database");


        return grpc::Status::OK;
    }

}