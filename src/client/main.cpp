
#include <iostream>
#include <memory>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <grpcpp/grpcpp.h>

#include <fmt/format.h>

#include "messaging/server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <google/protobuf/timestamp.pb.h>
#include <chrono>

#include <bsoncxx/oid.hpp>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;
using server::CreatureId;
using server::ListCreaturesResponse;
using server::CreatureFilter;

using spdlog::trace;
using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;

std::string ProtobufTimestampToHumanReadable(const google::protobuf::Timestamp& timestamp) {
    // Combine seconds and nanoseconds into a single duration
    auto seconds_duration = std::chrono::seconds(timestamp.seconds());
    auto nanoseconds_duration = std::chrono::nanoseconds(timestamp.nanos());

    // Convert the duration to a std::chrono::system_clock::time_point
    std::chrono::system_clock::time_point tp = std::chrono::system_clock::from_time_t(0) + seconds_duration;
    tp += std::chrono::duration_cast<std::chrono::system_clock::duration>(nanoseconds_duration);

    // Convert the std::chrono::system_clock::time_point to a std::time_t
    std::time_t time = std::chrono::system_clock::to_time_t(tp);

    // Convert the std::time_t to a std::tm
    std::tm tm{};
    gmtime_r(&time, &tm);

    // Format the std::tm into a human-readable string
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);

    // Return the formatted string
    return {buffer};
}

class CreatureServerClient {
public:
    explicit CreatureServerClient(const std::shared_ptr<Channel>& channel)
            : stub_(CreatureServer::NewStub(channel)) {}

    // Assembles the client's payload, sends it and presents the response back
    // from the server.
    Creature SearchCreatures(const std::string& name) {
        // Data we are sending to the server.
        CreatureName request;
        request.set_name(name);

        // Container for the data we expect from the server.
        Creature reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->SearchCreatures(&context, request, &reply);

        // Act upon its status.
        if (status.ok()) {
            debug("✅ search ok!");
            return reply;
        } else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("search not found! 🚫");
            return reply;
        } else {
            critical("search not okay: {}: {}", status.error_code(), status.error_message());
            return reply;
        }
    }

    Creature GetCreature(const CreatureId& id) {

        debug("in GetCreature() with {}");


        // Container for the data we expect from the server.
        Creature reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->GetCreature(&context, id, &reply);

        // Act upon its status.
        if (status.ok()) {
            debug("✅ get ok!");
            return reply;
        } else if(status.error_code() == grpc::StatusCode::NOT_FOUND) {
            info("get not found! 🚫");
            return reply;
        } else {
            critical("get not okay: {}: {}", status.error_code(), status.error_message());
            return reply;
        }
    }

    server::DatabaseInfo CreateCreature(const Creature& creature) {

        ClientContext context;
        server::DatabaseInfo reply;

        Status status = stub_->CreateCreature(&context, creature, &reply);

        if(status.ok()) {
            debug("got an OK from the server on save! ({})", reply.message());
        }
        else if(status.error_code() == grpc::StatusCode::ALREADY_EXISTS) {
           error("Creature {} already exists in the database", creature.name());
        }
        else {
           error("Unable to save a creature in the database: {} ({})",
                          status.error_message(), status.error_details());
        }

        return reply;

    }

    server::ListCreaturesResponse ListCreatures(const CreatureFilter& filter) {

        ClientContext context;
        server::ListCreaturesResponse reply;

        Status status = stub_->ListCreatures(&context, filter, &reply);

        if(status.ok()) {
            debug("got an OK from the server on a request to list all of the creatures");
        }
        else {
            error("An error happened while trying to list all of the creatures: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;
    }

    server::GetAllCreaturesResponse GetAllCreatures(const CreatureFilter& filter) {

        ClientContext context;
        server::GetAllCreaturesResponse reply;

        Status status = stub_->GetAllCreatures(&context, filter, &reply);

        if(status.ok()) {
            debug("got an OK from the server on a request to get all of the creatures");
        }
        else {
            error("An error happened while trying to list all of the creatures: {} ({})",
                  status.error_message(), status.error_details());
        }

        return reply;
    }

private:
    std::unique_ptr<CreatureServer::Stub> stub_;
};

google::protobuf::Timestamp time_point_to_protobuf_timestamp(const std::chrono::system_clock::time_point& time_point) {
    google::protobuf::Timestamp timestamp;
    auto seconds_since_epoch = std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch());
    auto nanos_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(time_point.time_since_epoch());
    timestamp.set_seconds(seconds_since_epoch.count());
    timestamp.set_nanos(nanos_since_epoch.count() % 1000000000); // Only store nanoseconds part
    return timestamp;
}




int main(int argc, char** argv) {

    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_level(spdlog::level::trace);

    // We indicate that the channel isn't authenticated (use of
    // InsecureChannelCredentials()).
    CreatureServerClient client(
            grpc::CreateChannel("localhost:6666", grpc::InsecureChannelCredentials()));

    info("Searching for a creature name that should exist...");
    Creature reply = client.SearchCreatures("Beaky1");
    info( "client gotten: {}, {}, {}, {}",
          reply.name(), reply.sacn_ip(), reply.dmx_base(), ProtobufTimestampToHumanReadable(reply.last_updated()));

    info("Searching for a creature that should NOT exist...");
    reply = client.SearchCreatures("Poop Face");
    trace("doing something with the reply so the compiler doesn't eat it... {}", reply.name());


    // Get the current time as a std::chrono::system_clock::time_point
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    // Convert the time_point to a google::protobuf::Timestamp
    google::protobuf::Timestamp current_timestamp = time_point_to_protobuf_timestamp(now);


    // Let's try to save one
    server::Creature creature = server::Creature();
    creature.set_name("Beaky8");
    creature.set_dmx_base(34);
    creature.set_number_of_motors(7);
    creature.set_universe(1);
    creature.set_sacn_ip("10.3.2.11");
    *creature.mutable_last_updated() = current_timestamp;

    for(int i = 0; i < 7; i++) {
        ::Creature::Motor *motor = creature.add_motors();

        motor->set_name(fmt::format("Motor {} 🦾", i));      // Toss in some UTF-8 for testing
        motor->set_min_value(253);
        motor->set_max_value(2934);
        motor->set_smoothing_value(0.932f);
        motor->set_number(i);

        if(i % 2 == 0)
            motor->set_type(::Creature::servo);
        else
            motor->set_type(::Creature::stepper);
    }

    client.CreateCreature(creature);
    info("create done");


    // Try to get a creature by ID
    std::string oid_string = "6431c48d6e9cc35e2d089263";
    info("attempting to search for ID {} in the database...", oid_string);

    bsoncxx::oid oid(oid_string);
    CreatureId creatureId;

    const char* oid_data = oid.bytes();
    creatureId.set__id(oid_data, bsoncxx::oid::k_oid_length);

    reply = client.GetCreature(creatureId);
    info("found creature named {} on a GetCreature call", reply.name());


    // Try to list all the creatures
    info("Now attempting to list all of the creatures!");

    CreatureFilter filter = CreatureFilter();
    filter.set_sortby(::server::SortBy::name);

    auto list = client.ListCreatures(filter);
     for(const auto& id : list.creaturesids() )
     {
         debug("Creature found {}", id.name());
     }


    // Get ALLLLLL of the creatures
    info("Now attempting to get ALLLLLLL of the creatures!");

    filter = CreatureFilter();
    filter.set_sortby(::server::SortBy::name);

    auto everyone = client.GetAllCreatures(filter);
    for(const auto& c : everyone.creatures() )
    {
        debug("Creature found {} with {} motors", c.name(), c.number_of_motors());
    }


    return 0;
}