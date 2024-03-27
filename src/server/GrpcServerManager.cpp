
#include <spdlog/spdlog.h>

#include "absl/strings/str_format.h"
#include <grpcpp/grpcpp.h>

#include "server/creature-server.h"
#include "server/logging/concurrentqueue.h"
#include "server/logging/creature_log_sink.h"
#include "util/StoppableThread.h"
#include "util/threadName.h"

#include "server/namespace-stuffs.h"

#include "GrpcServerManager.h"

namespace creatures {

    GrpcServerManager::GrpcServerManager(const std::string& serverAddress,
                                         uint16_t serverPort,
                                         ConcurrentQueue<LogItem> &logQueue) :
                                         serverAddress(std::move(serverAddress)),
                                         serverPort(serverPort),
                                         logQueue(logQueue) {
        info("GrpcServerManager created");
    }


    void GrpcServerManager::start() {
        debug("firing off gRPC server thread!");
        creatures::StoppableThread::start();
    }

    void GrpcServerManager::run() {

        setThreadName("GrpcServerManager::run");

        // Create our address
        std::string serverAddressWithPort = fmt::format("{}:{}", serverAddress, serverPort);
        debug("will listen on {}", serverAddressWithPort);

        CreatureServerImpl service(logQueue);

        ServerBuilder builder;
        builder.AddListeningPort(serverAddressWithPort, grpc::InsecureServerCredentials());
        builder.RegisterService(&service);

        grpcServer = builder.BuildAndStart();
        info("Server listening on {}", serverAddressWithPort);

        // Wait until we're asked to stop
        while (!stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }

        // If the server is still running, shut it down
        if (grpcServer) {

            // Signal a shutdown
            info("telling the grpcServer to stop");
            grpcServer->Shutdown();

            // Wait for it to actually stop
            debug("waiting...");
            grpcServer->Wait();
        }

        info("goodbye from the gRPC server!");

    }
}
