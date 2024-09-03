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
        static uint8_t getInterfaceIndex(const std::string& interfaceName);
        static std::string getVersion();
    };

}