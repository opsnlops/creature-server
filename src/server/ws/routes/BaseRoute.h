
#pragma once

#include <memory>
#include <vector>

#include "spdlog/spdlog.h"
#include <uWebSockets/App.h>

// Define a concept for a route
template<typename T>
concept RouteConcept = requires(T a, uWS::App& app) {
    { a.registerRoute(app) } -> std::same_as<void>;
};

namespace creatures :: ws {

    class BaseRoute {
    public:

        explicit BaseRoute(std::shared_ptr<spdlog::logger> logger) : logger(logger) {}
        virtual ~BaseRoute() = default;

        virtual void registerRoute(uWS::App& app) = 0;

    protected:
        std::shared_ptr<spdlog::logger> logger;
    };

}
