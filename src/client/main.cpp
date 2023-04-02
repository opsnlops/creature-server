
#include <iostream>
#include <memory>

#include <grpcpp/grpcpp.h>


#include "messaging/server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <google/protobuf/timestamp.pb.h>
#include <chrono>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;

using spdlog::info;
using spdlog::debug;
using spdlog::critical;


class CreatureServerClient {
public:
    explicit CreatureServerClient(const std::shared_ptr<Channel>& channel)
            : stub_(CreatureServer::NewStub(channel)) {}

    // Assembles the client's payload, sends it and presents the response back
    // from the server.
    Creature GetCreature(const std::string& name) {
        // Data we are sending to the server.
        CreatureName request;
        request.set_name(name);

        // Container for the data we expect from the server.
        Creature reply;

        // Context for the client. It could be used to convey extra information to
        // the server and/or tweak certain RPC behaviors.
        ClientContext context;

        // The actual RPC.
        Status status = stub_->GetCreature(&context, request, &reply);

        // Act upon its status.
        if (status.ok()) {
            debug("ok!");
            return reply;
        } else {
            critical("not okay: {}: {}", status.error_code(), status.error_message());
            return reply;
        }
    }

    server::DatabaseInfo CreateCreature(const Creature& creature) {

        ClientContext context;
        server::DatabaseInfo reply;

        Status status = stub_->CreateCreature(&context, creature, &reply);

        if(status.ok()) {
            debug("got an OK from the server on save! ({})", reply.message());
        } else {
            spdlog::error("Unable to save a creature in the database: {}", reply.message());
            spdlog::error("Mongo said: {}", status.error_message());
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
    Creature reply = client.GetCreature("Beaky");
    info( "client gotten: {}, {}, {}", reply.name(), reply.sacn_ip(), reply.dmx_base());


    // Get the current time as a std::chrono::system_clock::time_point
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    // Convert the time_point to a google::protobuf::Timestamp
    google::protobuf::Timestamp current_timestamp = time_point_to_protobuf_timestamp(now);


    // Let's try to save one
    server::Creature creature = server::Creature();
    creature.set_name("Beaky");
    creature.set_dmx_base(666);
    creature.set_number_of_motors(5);
    creature.set_universe(1);
    creature.set_sacn_ip("10.3.2.11");
    //creature.set_allocated_last_updated(&current_timestamp);

    client.CreateCreature(creature);
    info("save done?");

    return 0;
}