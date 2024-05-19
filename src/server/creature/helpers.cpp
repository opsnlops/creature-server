

#include <string>
#include <cstdlib>

#include <spdlog/spdlog.h>

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
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    bsoncxx::document::value Database::creatureToBson(const creatures::Creature &creature) {

        debug("converting a creature to bson");

        bsoncxx::builder::stream::document builder{};
        trace("builder made");

        try {

            builder << "_id" << stringToOid(creature.id)
                    << "name" << creature.name
                    << "channel_offset" << bsoncxx::types::b_int32{static_cast<int32_t>(creature.channel_offset)}
                    << "audio_channel" << bsoncxx::types::b_int32{static_cast<int32_t>(creature.audio_channel)};
            trace("fields added");

        }
        catch (const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("Problems making the document for a creature: {}", e.what());
            error(errorMessage);
            throw creatures::InternalError(errorMessage);
        }

        // All done, close up the doc!
        bsoncxx::document::value doc = builder.extract();
        trace("extract done");

        // Log this so I can see what was made
        trace("Built doc: {}", bsoncxx::to_json(doc));

        return doc;
    }



    creatures::Creature Database::creatureFromBson(const bsoncxx::document::view &doc) {

        trace("attempting to create a creature from a BSON document");


        Creature c;

        bsoncxx::document::element element = doc["id"];
        if (element && element.type() == bsoncxx::type::k_utf8) {
            bsoncxx::stdx::string_view string_value = element.get_string().value;
            c.id = std::string{string_value};
            trace("set the _id to {}", c.id);
        } else {
            throw creatures::DataFormatException("Field id was not a string in the database");
        }

        element = doc["name"];
        if (element && element.type() == bsoncxx::type::k_utf8) {
            bsoncxx::stdx::string_view string_value = element.get_string().value;
            c.name = std::string{string_value};
            trace("set the name to {}", c.name);
        } else {
            throw creatures::DataFormatException("Field name was not a string in the database");
        }



        // Number of audio channels
        element = doc["audio_channel"];
        if (element && element.type() == bsoncxx::type::k_int32) {
            c.audio_channel = element.get_int32().value;
            trace("set the audio channel to {}", c.audio_channel);
        } else {
            throw creatures::DataFormatException("Field audio_channel was not an int32 in the database");
        }


        // Channel offset
        element = doc["channel_offset"];
        if (element && element.type() == bsoncxx::type::k_int32) {
            c.channel_offset = element.get_int32().value;
            trace("set the channel offset to {}", c.channel_offset);
        } else {
            throw creatures::DataFormatException("Field channel_offset was not an int32 in the database");
        }


        debug("done loading creature");
        return c;
    }


    Result<creatures::Creature> Database::creatureFromJson(json creatureJson) {

        try {

            auto creature = Creature();
            creature.id = creatureJson["id"];
            creature.name = creatureJson["name"];
            creature.audio_channel = creatureJson["audio_channel"];
            creature.channel_offset = creatureJson["channel_offset"];

            if(creature.id.empty()) {
                std::string errorMessage = "Creature ID is empty";
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            if(creature.name.empty()) {
                std::string errorMessage = "Creature name is empty";
                warn(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            return Result<creatures::Creature>{creature};

        } catch ( const nlohmann::json::exception& e ) {
            std::string errorMessage = fmt::format("Error while converting JSON to Creature: {}", e.what());
            warn(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
    }
}