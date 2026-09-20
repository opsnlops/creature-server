
#pragma once

#include <iostream>

#include "spdlog/spdlog.h"

#include <oatpp/network/Server.hpp>

#include "server/database.h"
#include "server/transport/TransportServer.h"
#include "util/StoppableThread.h"

#include "AppComponent.h"
#include "controller/StaticController.h"
#include "util/MessageQueue.h"

namespace creatures ::ws {

class App : public StoppableThread, public transport::TransportServer {
  public:
    App();
    ~App();

    void start() override;
    void shutdown() override;

  protected:
    void run() override;

  private:
    std::shared_ptr<spdlog::logger> internalLogger;
    std::thread pingThread;
    std::thread messageLoopThread;
};

} // namespace creatures::ws
