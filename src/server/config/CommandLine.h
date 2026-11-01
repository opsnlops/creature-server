#pragma once

#include "server/config/Configuration.h"

namespace creatures {

class CommandLine {

  public:
    CommandLine() = default;
    ~CommandLine() = default;

    static std::shared_ptr<Configuration> parseCommandLine(int argc, char **argv);

  private:
    static void listSoundDevices();
    static void listNetworkDevices();
    /// Resolve a NIC name to its OS interface index. uint16_t to match what
    /// Configuration stores; an index too large to represent throws rather
    /// than being truncated into a valid-looking one (issue #238).
    static uint16_t getInterfaceIndex(const std::string &interfaceName);
    static std::string getVersion();
};

} // namespace creatures