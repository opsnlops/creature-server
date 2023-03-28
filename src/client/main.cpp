
#include <iostream>
#include <memory>

#include <grpcpp/grpcpp.h>


#include "messaging/server.grpc.pb.h"



using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;


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
            printf("ok!\n");
            return reply;
        } else {
            printf("not okay\n");
            std::cout << status.error_code() << ": " << status.error_message()
                      << std::endl;
            return reply;
        }
    }

private:
    std::unique_ptr<CreatureServer::Stub> stub_;
};

int main(int argc, char** argv) {

    // We indicate that the channel isn't authenticated (use of
    // InsecureChannelCredentials()).
    CreatureServerClient client(
            grpc::CreateChannel("localhost:6666", grpc::InsecureChannelCredentials()));
    Creature reply = client.GetCreature("Beaky");
    printf("client gotten: %s, %s, %u\n", reply.name().c_str(), reply.sacn_ip().c_str(), reply.dmx_base());


    return 0;
}