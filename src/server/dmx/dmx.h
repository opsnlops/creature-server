
#pragma once

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

extern "C" {
    #include <e131.h>
}

namespace creatures {

    class DMX {

    public:
        DMX();
        ~DMX();

        void init(std::string ip_address, bool use_multicast, uint32_t universe, uint32_t number_of_motors);
        void send(const std::vector<uint8_t>& data);

        // Returns the current sequence number of the packets we're sending
        [[nodiscard]] uint8_t getSequenceNumber() const;

    private:

        std::string hostBanner;
        std::string ip_address;
        bool use_multicast;
        uint32_t hostBannerLength;
        int socketFd{};
        uint32_t number_of_motors{};
        uint32_t universe{};

        uint32_t dmx_offset{};
        e131_packet_t packet{};
        e131_addr_t dest{};
    };
}