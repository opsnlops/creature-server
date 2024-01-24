#pragma once

#include <argparse/argparse.hpp>

#include "server/config/Configuration.h"

namespace creatures {

    class CommandLine {

    public:
        CommandLine() = default;
        ~CommandLine() = default;
        std::shared_ptr<Configuration> parseCommandLine(int argc, char **argv);

    private:
        static void listSoundDevices();
        static void listNetworkDevices();
        std::string getVersion();
    };

}