
#pragma once

#include <string>

#include "e131.h"

namespace creatures {

    class DMX {

    public:
        DMX();
        ~DMX();

        void init(std::string ip_address, uint32_t universe, uint32_t number_of_motors);
        void send(const uint8_t data[]);

    private:

        std::string hostBanner;
        std::string ip_address;
        uint32_t hostBannerLength;
        int socketFd{};
        uint32_t number_of_motors{};
        uint32_t universe{};

        uint32_t dmx_offset{};
        e131_packet_t packet{};
        e131_addr_t dest{};

    };
}