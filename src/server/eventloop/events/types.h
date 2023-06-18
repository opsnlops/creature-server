
#pragma once

#include <cstdlib>
#include <vector>

#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"


namespace creatures {

    class TickEvent : public EventBase<TickEvent> {
    public:
        using EventBase::EventBase;
        void executeImpl();
    };

    class DMXEvent : public EventBase<DMXEvent> {
    public:
        using EventBase::EventBase;
        void executeImpl();

        // Used to create a DMX object if it doesn't exist
        std::string clientIP;
        uint32_t dmxUniverse;
        uint32_t dmxOffset;
        uint32_t numMotors;

        // Used every time to send data
        std::vector<uint8_t> data;
    };

    class MusicEvent : public EventBase<MusicEvent> {
    public:
        using EventBase::EventBase;
        MusicEvent(int frameNumber, std::string filePath);
        void executeImpl();

        static int initSDL();
        static int locateAudioDevice();
        static std::string getSoundFileLocation();
        static void listAudioDevices();

    private:
        std::string filePath;
        std::mutex sdl_mutex;
    };

}