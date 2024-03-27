
#pragma once

#include <spdlog/spdlog.h>

#include "absl/strings/str_format.h"
#include <grpcpp/grpcpp.h>

#include "server/logging/concurrentqueue.h"
#include "server/logging/creature_log_sink.h"
#include "util/StoppableThread.h"

using moodycamel::ConcurrentQueue;

namespace creatures {

    /**
     * This class manages our gRPC server, allowing us to start and stop it cleaning
     */
    class GrpcServerManager : public StoppableThread {

    public:
        GrpcServerManager(const std::string& serverAddress, uint16_t serverPort, ConcurrentQueue<LogItem> &logQueue);

        void start() override;

    protected:
        void run() override;

    private:
        std::string serverAddress;
        uint16_t serverPort;
        ConcurrentQueue<LogItem>& logQueue;

        // We own this. Do not share. 😅
        std::unique_ptr<grpc::Server> grpcServer;
    };

} // creatures

