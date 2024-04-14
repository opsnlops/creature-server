
#include "server/config.h"

#include <string>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <chrono>

#include "spdlog/spdlog.h"

#include "server.pb.h"
#include "server/database.h"
#include "exception/exception.h"


#include <google/protobuf/timestamp.pb.h>

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
    bsoncxx::document::value Database::creatureToBson(const server::Creature *creature, bool assignNewId) {
        using bsoncxx::builder::stream::document;
        using bsoncxx::builder::stream::finalize;
        using std::chrono::system_clock;

        debug("converting a creature to bson");

        bsoncxx::builder::stream::document builder{};
        trace("builder made");

        // Generate a new ID
        bsoncxx::oid id;
        if (!assignNewId) {
            trace("reusing old ID");
            id = bsoncxx::oid(creature->_id().data(), bsoncxx::oid::k_oid_length);
            debug("parsed ID to: {}", id.to_string());
        }

        // Convert the protobuf Timestamp to a std::chrono::system_clock::time_point
        system_clock::time_point last_updated = protobufTimestampToTimePoint(creature->last_updated());
        try {

            builder << "_id" << id
                    << "name" << creature->name()
                    << "last_updated" << bsoncxx::types::b_date{last_updated}
                    << "channel_offset" << bsoncxx::types::b_int32{static_cast<int32_t>(creature->channel_offset())}
                    << "audio_channel" << bsoncxx::types::b_int32{static_cast<int32_t>(creature->audio_channel())}
                    << "notes" << creature->notes();
            trace("fields added");

        }
        catch (const mongocxx::exception &e) {
            error("Problems making the document: {}", e.what());
        }

        // All done, close up the doc!
        bsoncxx::document::value doc = builder.extract();
        trace("extract done");

        // Log this so I can see what was made
        trace("Built doc: {}", bsoncxx::to_json(doc));

        return doc;
    }


    void Database::creatureFromBson(const bsoncxx::document::view &doc, server::Creature *creature) {

        trace("attempting to create a creature from a BSON document");


        bsoncxx::document::element element = doc["_id"];
        if (element && element.type() == bsoncxx::type::k_oid) {
            const bsoncxx::oid &oid = element.get_oid().value;
            const char *oid_data = oid.bytes();
            creature->set__id(oid_data, bsoncxx::oid::k_oid_length);
            trace("set the _id to {}", oid.to_string());
        } else {
            throw creatures::DataFormatException("Field _id was not a bsoncxx::oid in the database");
        }

        element = doc["name"];
        if (element && element.type() == bsoncxx::type::k_utf8) {
            bsoncxx::stdx::string_view string_value = element.get_string().value;
            creature->set_name(std::string{string_value});
            trace("set the name to {}", creature->name());
        } else {
            throw creatures::DataFormatException("Field name was not a string in the database");
        }


        // Last updated
        element = doc["last_updated"];
        *creature->mutable_last_updated() = convertMongoDateToProtobufTimestamp(element);


        // Channel Offset
        element = doc["channel_offset"];
        if (element && element.type() == bsoncxx::type::k_int32) {
            int32_t int32_value = element.get_int32().value;
            creature->set_channel_offset(int32_value);
            trace("set the channel offset value to {}", creature->channel_offset());
        } else {
            throw creatures::DataFormatException("Field 'channel_offset' was not an int32 in the database");
        }

        // Number of motors
        element = doc["audio_channel"];
        if (element && element.type() == bsoncxx::type::k_int32) {
            int32_t int32_value = element.get_int32().value;
            creature->set_audio_channel(int32_value);
            trace("set the audio channel to {}", creature->audio_channel());
        } else {
            throw creatures::DataFormatException("Field audio_channel was not an int32 in the database");
        }


        // Notes
        element = doc["notes"];
        if (element && element.type() == bsoncxx::type::k_utf8) {
            bsoncxx::stdx::string_view string_value = element.get_string().value;
            creature->set_notes(std::string{string_value});
            trace("set the notes to {}", creature->notes());
        } else {
            throw creatures::DataFormatException("Field notes was not a string in the database");
        }

        debug("done loading creature");
    }


    void
    Database::creatureIdentifierFromBson(const bsoncxx::document::view &doc, server::CreatureIdentifier *identifier) {

        trace("attempting to create a creatureIdentifier from a BSON document");

        bsoncxx::document::element element = doc["_id"];
        if (element && element.type() == bsoncxx::type::k_oid) {
            const bsoncxx::oid &oid = element.get_oid().value;
            const char *oid_data = oid.bytes();
            identifier->set__id(oid_data, bsoncxx::oid::k_oid_length);
            trace("set the _id to {}", oid.to_string());
        } else {
            throw creatures::DataFormatException("Field '_id' was not a bsoncxx::oid in the database");
        }

        element = doc["name"];
        if (element && element.type() == bsoncxx::type::k_utf8) {
            bsoncxx::stdx::string_view string_value = element.get_string().value;
            identifier->set_name(std::string{string_value});
            trace("set the name to {}", identifier->name());
        } else {
            throw creatures::DataFormatException("Field 'name' was not a string in the database");
        }

        debug("done loading creatureIdentifier");
    }


    std::chrono::system_clock::time_point
    Database::protobufTimestampToTimePoint(const google::protobuf::Timestamp &timestamp) {
        using std::chrono::duration_cast;
        using std::chrono::nanoseconds;
        using std::chrono::seconds;
        using std::chrono::system_clock;

        auto duration = seconds{timestamp.seconds()} + nanoseconds{timestamp.nanos()};
        return system_clock::time_point{duration_cast<system_clock::duration>(duration)};
    }

    google::protobuf::Timestamp
    Database::convertMongoDateToProtobufTimestamp(const bsoncxx::document::element &mongo_date_element) {
        // Make sure the input BSON element has a Timestamp type
        if (mongo_date_element.type() != bsoncxx::type::k_date) {
            error("element type is not a k_date, cannot convert it to a protobuf timestamp. Type is: {}",
                  bsoncxx::to_string(mongo_date_element.type()));
            throw creatures::DataFormatException(
                    fmt::format(
                            "Element type is not a k_timestamp, cannot convert it to a protobuf timestamp. Found type was: {}",
                            bsoncxx::to_string(mongo_date_element.type())));
        }

        // Access the BSON Date value as a bsoncxx::types::b_date type
        bsoncxx::types::b_date mongo_date = mongo_date_element.get_date();

        // Convert the bsoncxx::types::b_date value to a std::chrono::milliseconds type
        std::chrono::milliseconds millis_since_epoch = mongo_date.value;

        // Convert the std::chrono::milliseconds to seconds and nanoseconds
        std::int64_t seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(millis_since_epoch).count();

        // Compute nanoseconds using a larger integer type to avoid narrowing conversion
        std::int64_t nanoseconds_long = (std::abs(millis_since_epoch.count()) % 1000) * 1000000LL;

        // Safely cast the result to std::int32_t
        auto nanoseconds = static_cast<std::int32_t>(nanoseconds_long);

        // Create a Protobuf Timestamp object
        google::protobuf::Timestamp protobuf_timestamp;

        // Set the Protobuf timestamp's seconds and nanoseconds fields
        protobuf_timestamp.set_seconds(seconds_since_epoch);
        protobuf_timestamp.set_nanos(nanoseconds);

        return protobuf_timestamp;
    }
}