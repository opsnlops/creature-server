
#include "server/config.h"

#include <string>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <chrono>

#include "spdlog/spdlog.h"

#include "server/database.h"
#include "exception/exception.h"
#include "util/helpers.h"

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/pool.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/array/element.hpp>
#include <mongocxx/cursor.hpp>

#include <bsoncxx/builder/stream/document.hpp>

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
                    << "audio_channel" << bsoncxx::types::b_int32{static_cast<int32_t>(creature.audio_channel)}
                    << "notes" << creature.notes;
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

        bsoncxx::document::element element = doc["_id"];
        if (element && element.type() == bsoncxx::type::k_oid) {
            trace("_id is valid and is {} bytes", element.get_oid().value.size());
            const bsoncxx::oid &oid = element.get_oid().value;
            const char *oid_data = oid.bytes();

            // Make sure we only have the data we're expecting
            char byteData[element.get_oid().value.size()];
            std::copy(oid_data, oid_data + element.get_oid().value.size(), byteData);

            c.id = bytesToString(byteData);
            trace("set the _id to {}", c.id);
        } else {
            throw creatures::DataFormatException("Field _id was not a bsoncxx::oid in the database");
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


        // Notes
        element = doc["notes"];
        if (element && element.type() == bsoncxx::type::k_utf8) {
            bsoncxx::stdx::string_view string_value = element.get_string().value;
            c.notes = std::string{string_value};
            trace("set the notes to {}", c.notes);
        } else {
            throw creatures::DataFormatException("Field notes was not a string in the database");
        }

        debug("done loading creature");
        return c;
    }


//    void
//    Database::creatureIdentifierFromBson(const bsoncxx::document::view &doc, server::CreatureIdentifier *identifier) {
//
//        trace("attempting to create a creatureIdentifier from a BSON document");
//
//        bsoncxx::document::element element = doc["_id"];
//        if (element && element.type() == bsoncxx::type::k_oid) {
//            const bsoncxx::oid &oid = element.get_oid().value;
//            const char *oid_data = oid.bytes();
//            identifier->set__id(oid_data, bsoncxx::oid::k_oid_length);
//            trace("set the _id to {}", oid.to_string());
//        } else {
//            throw creatures::DataFormatException("Field '_id' was not a bsoncxx::oid in the database");
//        }
//
//        element = doc["name"];
//        if (element && element.type() == bsoncxx::type::k_utf8) {
//            bsoncxx::stdx::string_view string_value = element.get_string().value;
//            identifier->set_name(std::string{string_value});
//            trace("set the name to {}", identifier->name());
//        } else {
//            throw creatures::DataFormatException("Field 'name' was not a string in the database");
//        }
//
//        debug("done loading creatureIdentifier");
//    }



}