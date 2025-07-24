
#pragma once

#include <iostream>

#include "spdlog/spdlog.h"

#include <oatpp/network/Server.hpp>

#include "server/database.h"
#include "util/StoppableThread.h"

#include "AppComponent.h"
#include "controller/StaticController.h"
#include "util/MessageQueue.h"

namespace creatures ::ws {

class App : public StoppableThread {
  public:
    App();
    ~App() = default;

    void start() override;

  protected:
    void run() override;

  private:
    std::shared_ptr<spdlog::logger> internalLogger;
};

} // namespace creatures::ws