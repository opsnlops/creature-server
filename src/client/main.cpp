
#include <iostream>
#include <memory>

#include <grpcpp/grpcpp.h>


#include "messaging/server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

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
    CreatureServerClient(std::shared_ptr<Channel> channel)
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

private:
    std::unique_ptr<CreatureServer::Stub> stub_;
};

int main(int argc, char** argv) {

    auto console = spdlog::stdout_color_mt("console");

    // We indicate that the channel isn't authenticated (use of
    // InsecureChannelCredentials()).
    CreatureServerClient client(
            grpc::CreateChannel("localhost:6666", grpc::InsecureChannelCredentials()));
    Creature reply = client.GetCreature("Beaky");
    info( "client gotten: {}, {}, {}", reply.name(), reply.sacn_ip(), reply.dmx_base());


    return 0;
}