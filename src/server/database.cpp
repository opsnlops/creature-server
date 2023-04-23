
#include <string>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <chrono>

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/database.h"
#include "exception/exception.h"

#include <fmt/format.h>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/timestamp.pb.h>

#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/pool.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/array/element.hpp>
#include <mongocxx/cursor.hpp>
#include <bsoncxx/exception/exception.hpp>

#include <bsoncxx/builder/stream/document.hpp>


using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;
using spdlog::trace;

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    Database::Database(mongocxx::pool &pool) : pool(pool) {

        info("starting up database connection");
    }


    mongocxx::collection Database::getCollection(const std::string &collectionName) {

        debug("connecting to a collection");

        // Acquire a MongoDB client from the pool
        auto client = pool.acquire();
        auto collection = (*client)[DB_NAME][collectionName];

        return collection;

    }

    grpc::Status Database::ping() {

        // Ping the database.
        const auto ping_cmd = make_document(kvp("ping", 1));

        //auto client = pool.acquire();
        //mongocxx::database db = client[DB_NAME];
        //db.run_command(ping_cmd.view());

        return grpc::Status::OK;

    }

    grpc::Status Database::getAllCreatures(const CreatureFilter *filter, GetAllCreaturesResponse *creatureList) {
        grpc::Status status;
        info("getting all of the creatures");

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection obtained");

        document query_doc{};
        document sort_doc{};

        switch (filter->sortby()) {
            case server::SortBy::number:
                sort_doc << "number" << 1;
                debug("sorting by number");
                break;

                // Default is by name
            default:
                sort_doc << "name" << 1;
                debug("sorting by name");
                break;
        }

        mongocxx::options::find opts{};
        opts.sort(sort_doc.view());
        mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

        for (auto &&doc: cursor) {

            auto creature = creatureList->add_creatures();
            creatureFromBson(doc, creature);

            debug("loaded {}", creature->name());
        }

        status = grpc::Status::OK;
        return status;

    }

    grpc::Status Database::listCreatures(const CreatureFilter *filter, ListCreaturesResponse *creatureList) {

        grpc::Status status;
        info("getting the list of creatures");

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection obtained");

        document query_doc{};
        document projection_doc{};
        document sort_doc{};

        switch (filter->sortby()) {
            case server::SortBy::number:
                sort_doc << "number" << 1;
                debug("sorting by number");
                break;

                // Default is by name
            default:
                sort_doc << "name" << 1;
                debug("sorting by name");
                break;
        }

        // We only want the name and _id
        projection_doc << "_id" << 1 << "name" << 1;

        mongocxx::options::find opts{};
        opts.projection(projection_doc.view());
        opts.sort(sort_doc.view());
        mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

        for (auto &&doc: cursor) {

            auto creatureId = creatureList->add_creaturesids();
            creatureIdentifierFromBson(doc, creatureId);

            debug("loaded {}", creatureId->name());

        }

        status = grpc::Status::OK;

        return status;
    }


    grpc::Status Database::searchCreatures(const CreatureName *creatureName, Creature *creature) {

        grpc::Status status;
        if (creatureName->name().empty()) {
            info("an attempt to search for an empty name was made");
            throw InvalidArgumentException("unable to search for Creatures because the name was empty");
        }

        // Okay, we know we have a non-empty name
        std::string name = creatureName->name();
        debug("attempting to search for a creature named {}", name);

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection located");

        try {
            // Create a filter BSON document to match the target document
            auto filter = bsoncxx::builder::stream::document{} << "name" << name << bsoncxx::builder::stream::finalize;

            // Find the document with the matching _id field
            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());

            if (!result) {
                info("no creatures named '{}' found", name);
                throw creatures::CreatureNotFoundException(fmt::format("no creatures named '{}' found", name));
            }

            // Unwrap the optional to obtain the bsoncxx::document::value
            bsoncxx::document::value found_document = *result;
            creatureFromBson(found_document, creature);

            debug("find completed!");

            return grpc::Status::OK;
        }
        catch (const mongocxx::exception &e) {
            critical("an unhandled error happened while searching for a creature: {}", e.what());
            throw InternalError(
                    fmt::format("an unhandled error happened while searching for a creature: {}", e.what()));
        }
    }

    grpc::Status Database::getCreature(const CreatureId *creatureId, Creature *creature) {

        grpc::Status status;
        if (creatureId->_id().empty()) {
            info("an empty creatureID was passed into getCreature()");
            throw InvalidArgumentException("unable to get a creature because the id was empty");
        }

        // Convert the ID into MongoID's ID
        bsoncxx::oid id = bsoncxx::oid(creatureId->_id().data(), 12);
        debug("attempting to search for a creature by ID: {}", id.to_string());

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection located");

        try {

            // Create a filter BSON document to match the target document
            auto filter = bsoncxx::builder::stream::document{} << "_id" << id << bsoncxx::builder::stream::finalize;
            trace("filter doc: {}", bsoncxx::to_json(filter));

            // Find the document with the matching _id field
            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());

            if (!result) {
                info("no creature with ID '{}' found", id.to_string());
                throw creatures::CreatureNotFoundException(fmt::format("no creature id '{}' found", id.to_string()));
            }

            // Unwrap the optional to obtain the bsoncxx::document::value
            bsoncxx::document::value found_document = *result;
            creatureFromBson(found_document, creature);

            debug("get completed!");

            return grpc::Status::OK;
        }
        catch (const mongocxx::exception &e) {
            critical("an unhandled error happened while loading a creature by ID: {}", e.what());
            throw InternalError(
                    fmt::format("an unhandled error happened while loading a creature by ID: {}", e.what()));
        }
    }

    grpc::Status Database::createCreature(const Creature *creature, server::DatabaseInfo *reply) {

        info("attempting to save a creature in the database");

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection made");

        grpc::Status status;

        debug("name: {}", creature->name().c_str());
        try {
            auto doc_value = creatureToBson(creature, true);
            trace("doc_value made");
            collection.insert_one(doc_value.view());
            trace("run_command done");

            info("saved creature in the database");

            status = grpc::Status::OK;
            reply->set_message("saved thingy in the thingy");

        }
        catch (const mongocxx::exception &e) {
            // Was this an attempt to make a duplicate creature?
            if (e.code().value() == 11000) {
                error("attempted to insert a duplicate Creature in the database for id {}", creature->_id());
                status = grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
                reply->set_message("Unable to create new creature");
                reply->set_help(fmt::format("ID {} already exists", creature->_id()));
            } else {
                critical("Error updating database: {}", e.what());
                status = grpc::Status(grpc::StatusCode::UNKNOWN, e.what(), fmt::to_string(e.code().value()));
                reply->set_message(
                        fmt::format("Unable to create Creature in database: {} ({})", e.what(), e.code().value()));
                reply->set_help(e.code().message());
            }

        }

        return status;
    }

    grpc::Status Database::updateCreature(const Creature *creature, server::DatabaseInfo *reply) {

        debug("attempting to update a creature in the database");

        auto collection = getCollection(COLLECTION_NAME);
        trace("collection made");

        grpc::Status status;

        debug("name: {}", creature->name().c_str());
        try {
            auto doc_value = creatureToBson(creature, false);
            trace("doc_value made");

            auto filter = bsoncxx::builder::stream::document{} << "_id" << creature->_id()
                                                               << bsoncxx::builder::stream::finalize;
            auto update =
                    bsoncxx::builder::stream::document{} << "$set" << doc_value << bsoncxx::builder::stream::finalize;

            mongocxx::stdx::optional<mongocxx::result::update> result =
                    collection.update_one(filter.view(), update.view());
            trace("update_one done");

            if (result) {
                debug("Matched {} documents", result->matched_count());
                debug("Modified {} documents", result->modified_count());
                reply->set_message("updated the creature in the database");
                status = grpc::Status::OK;
            } else {
                error("No documents matched the filter on update");
                reply->set_message("Unable to update, creature ID not found");
                reply->set_help(fmt::format("ID {} id not found in the database", creature->_id()));
                status = grpc::Status(grpc::StatusCode::NOT_FOUND, "Unable to update, creature ID not found");
            }
        }
        catch (const mongocxx::exception &e) {
            critical("Error updating database: {}", e.what());
            status = grpc::Status(grpc::StatusCode::UNKNOWN, e.what(), fmt::to_string(e.code().value()));
            reply->set_message(
                    fmt::format("Unable to update Creature in database: {} ({})", e.what(), e.code().value()));
            reply->set_help(e.code().message());
        }

        return status;
    }

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
            id = bsoncxx::oid(creature->_id());
        }

        // Convert the protobuf Timestamp to a std::chrono::system_clock::time_point
        system_clock::time_point last_updated = protobufTimestampToTimePoint(creature->last_updated());
        try {

            builder << "_id" << id
                    << "name" << creature->name()
                    << "sacn_ip" << creature->sacn_ip()
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
                    trace("set the motor type to {}", motor->type());

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