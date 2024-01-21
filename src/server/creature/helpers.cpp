
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
    bsoncxx::document::value Database::creatureToBson(const Creature *creature, bool assignNewId) {
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
                    << "sacn_ip" << creature->sacn_ip()
                    << "use_multicast" << bsoncxx::types::b_bool{creature->use_multicast()}
                    << "type" << bsoncxx::types::b_int32{static_cast<int32_t>(creature->type())}
                    << "last_updated" << bsoncxx::types::b_date{last_updated}
                    << "universe" << bsoncxx::types::b_int32{static_cast<int32_t>(creature->universe())}
                    << "dmx_base" << bsoncxx::types::b_int32{static_cast<int32_t>(creature->dmx_base())}
                    << "number_of_motors"
                    << bsoncxx::types::b_int32{static_cast<int32_t>(creature->number_of_motors())};
            trace("non-array fields added");

            auto array_builder = builder << "motors" << bsoncxx::builder::stream::open_array;
            trace("array_builder made");
            for (int j = 0; j < creature->motors_size(); j++) {

                const server::Creature_Motor &motor = creature->motors(j);
                trace("adding motor {}", j);

                // Convert the double value to a std::string with fixed precision. Boo floating point!
                std::ostringstream str_smoothing_value;
                str_smoothing_value << std::fixed << std::setprecision(4) << motor.smoothing_value();
                std::string smoothing_value_str = str_smoothing_value.str();
                bsoncxx::types::b_decimal128 decimal_smoothing_value(smoothing_value_str);

                // Assign a new ID
                bsoncxx::oid motorId;
                array_builder = array_builder << bsoncxx::builder::stream::open_document
                                              << "_id" << motorId
                                              << "name" << motor.name()
                                              << "type" << bsoncxx::types::b_int32{static_cast<int32_t>(motor.type())}
                                              << "number"
                                              << bsoncxx::types::b_int32{static_cast<int32_t>(motor.number())}
                                              << "max_value"
                                              << bsoncxx::types::b_int32{static_cast<int32_t>(motor.max_value())}
                                              << "min_value"
                                              << bsoncxx::types::b_int32{static_cast<int32_t>(motor.min_value())}
                                              << "smoothing_value" << decimal_smoothing_value
                                              << bsoncxx::builder::stream::close_document;
            }
            trace("done adding motors");
            array_builder << bsoncxx::builder::stream::close_array;

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


    void Database::creatureFromBson(const bsoncxx::document::view &doc, Creature *creature) {

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

        element = doc["sacn_ip"];
        if (element && element.type() == bsoncxx::type::k_utf8) {
            bsoncxx::stdx::string_view string_value = element.get_string().value;
            creature->set_sacn_ip(std::string{string_value});
            trace("set the sacn_ip to {}", creature->sacn_ip());
        } else {
            throw creatures::DataFormatException("Field sacn_ip was not a string in the database");
        }

        element = doc["use_multicast"];
        if (!element) {
            info("defaulting `use_multicast` to false");
            creature->set_use_multicast(false);
        }
        else if (element.type() == bsoncxx::type::k_bool) {
            bool bool_value = element.get_bool().value;
            creature->set_use_multicast(bool_value);
            trace("set the use_multicast to {}", creature->use_multicast());
        } else {
            throw creatures::DataFormatException("Field use_multicast was not a bool in the database");
        }

        // DMX Universe
        element = doc["universe"];
        if (element && element.type() == bsoncxx::type::k_int32) {
            int32_t int32_value = element.get_int32().value;
            creature->set_universe(int32_value);
            trace("set the DMX universe to {}", creature->universe());
        } else {
            throw creatures::DataFormatException("Field universe was not an int32 in the database");
        }

        // DMX Base Channel
        element = doc["dmx_base"];
        if (element && element.type() == bsoncxx::type::k_int32) {
            int32_t int32_value = element.get_int32().value;
            creature->set_dmx_base(int32_value);
            trace("set the DMX base value to {}", creature->dmx_base());
        } else {
            throw creatures::DataFormatException("Field dmx_base was not an int32 in the database");
        }

        // Number of motors
        element = doc["number_of_motors"];
        if (element && element.type() == bsoncxx::type::k_int32) {
            int32_t int32_value = element.get_int32().value;
            creature->set_number_of_motors(int32_value);
            trace("set the number of motors to {}", creature->number_of_motors());
        } else {
            throw creatures::DataFormatException("Field number_of_motors was not an int32 in the database");
        }

        // Motor type
        element = doc["type"];
        if (element && element.type() != bsoncxx::type::k_int32) {
            error("creature field 'type' is not an int");
            throw creatures::DataFormatException("creature field 'type' is not an int in the database");
        }

        // Check and see if it's accurate
        int32_t creatureType = element.get_int32().value;
        if (!server::CreatureType_IsValid((creatureType))) {
            error("creature field 'type' does not map to our enum: {}", creatureType);
            throw creatures::DataFormatException(
                    fmt::format("creature field 'type' does not map to our enum: {}", creatureType));
        }
        creature->set_type(static_cast<CreatureType>(creatureType));



        // Last updated
        element = doc["last_updated"];
        *creature->mutable_last_updated() = convertMongoDateToProtobufTimestamp(element);


        // Number of motors
        element = doc["motors"];
        if (element && element.type() == bsoncxx::type::k_array) {
            bsoncxx::array::view array_view = element.get_array().value;

            // Iterate through the array of objects
            for (const bsoncxx::array::element &obj_element: array_view) {
                // Ensure the element is a document (BSON object)
                if (obj_element.type() == bsoncxx::type::k_document) {
                    bsoncxx::document::view obj = obj_element.get_document().value;

                    server::Creature_Motor *motor = creature->add_motors();

                    // Motor name
                    element = obj["name"];
                    if (element && element.type() != bsoncxx::type::k_utf8) {
                        error("motor field 'name' was not a string in the database");
                        throw creatures::DataFormatException("motor field 'name' was not a string in the database");
                    }
                    motor->set_name(std::string{element.get_string().value});
                    trace("set the motor name to {}", motor->name());

                    // Motor type
                    element = obj["type"];
                    if (element && element.type() != bsoncxx::type::k_int32) {
                        error("motor field 'type' is not an int");
                        throw creatures::DataFormatException("motor field 'type' is not an int in the database");
                    }

                    int32_t motor_type = element.get_int32().value;

                    // Check if the integer value is a valid MotorType enum value
                    if (!server::Creature_MotorType_IsValid((motor_type))) {
                        error("motor field 'type' does not map to our enum: {}", motor_type);
                        throw creatures::DataFormatException(
                                fmt::format("motor field 'type' does not map to our enum: {}", motor_type));
                    }

                    // Cast the int to the right value for our enum
                    motor->set_type(static_cast<Creature::MotorType>(motor_type));
                    trace("set the motor type to {}", static_cast<int32_t>(motor->type()));

                    // Motor number
                    element = obj["number"];
                    if (element && element.type() == bsoncxx::type::k_int32) {
                        int32_t int32_value = element.get_int32().value;
                        motor->set_number(int32_value);
                        trace("set the motor number to {}", motor->number());
                    } else {
                        error("motor field 'number' was not an int in the database");
                        throw creatures::DataFormatException("motor field 'number' was not an int in the database");
                    }

                    // Max Value
                    element = obj["max_value"];
                    if (element && element.type() != bsoncxx::type::k_int32) {
                        error("motor value 'max_value' is not an int");
                        throw creatures::DataFormatException("motor value 'max_value' is not an int");
                    }
                    motor->set_max_value(element.get_int32().value);
                    trace("set the motor max_value to {}", motor->max_value());


                    // Min Value
                    element = obj["min_value"];
                    if (element && element.type() != bsoncxx::type::k_int32) {
                        error("motor value 'min_value' is not an int");
                        throw creatures::DataFormatException("motor value 'min_value' is not an int");
                    }
                    motor->set_min_value(element.get_int32().value);
                    trace("set the motor min_value to {}", motor->min_value());

                    // Smoothing
                    element = obj["smoothing_value"];
                    if (element && element.type() != bsoncxx::type::k_decimal128) {
                        error("motor value 'smoothing_value' is not a k_decimal128");
                        throw creatures::DataFormatException("motor value 'smoothing_value' is not a k_decimal128");
                    }

                    /*
                     * Note to future me!
                     *
                     * I'm using the Decimal128 format here since Mongo wants to turn a double like
                     * 0.95 into 0.94999999234, which looks goofy in the UI. There's no bridge from
                     * Decimal128 to double without going into a string first, so that's where this
                     * goes.
                     */
                    bsoncxx::types::b_decimal128 decimal_value = element.get_decimal128();
                    std::string decimal_str = decimal_value.value.to_string();

                    motor->set_smoothing_value(std::stod(decimal_str));
                    trace("set the motor smoothing_value to {}", motor->smoothing_value());
                }


            }

        } else {
            error("The field 'motors' was not an array in the database");
            throw creatures::DataFormatException("Field 'motors' was not an array in the database");
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