
#pragma once

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
    #include <e131.h>
}

namespace creatures::e131 {

    class E131Server {

    public:

        E131Server() = default;
        ~E131Server() = default;

        void init();

    private:
        std::shared_ptr<spdlog::logger> logger;


    };

} // creatures::e131

