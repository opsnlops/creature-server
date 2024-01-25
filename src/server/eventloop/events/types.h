
#pragma once

#include <cstdlib>
#include <vector>

#include "server/config.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"

#include "server/namespace-stuffs.h"


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

        uint32_t dmxUniverse;
        uint32_t dmxOffset;
        uint32_t numMotors;

        // Used every time to send data
        std::vector<uint8_t> data;
    };

    class MusicEvent : public EventBase<MusicEvent> {
    public:
        using EventBase::EventBase;
        MusicEvent(uint64_t frameNumber, std::string filePath);
        void executeImpl();

        static int initSDL();
        static int locateAudioDevice();
        static void listAudioDevices();

    private:
        std::string filePath;
        std::mutex sdl_mutex;
    };

    class PlaylistEvent : public EventBase<PlaylistEvent> {
    public:
        using EventBase::EventBase;
        PlaylistEvent(uint64_t frameNumber, std::string creatureIdString);
        void executeImpl();

    private:
        std::string creatureIdString;
    };


    enum class StatusLight : uint8_t {
        Running = SERVER_RUNNING_GPIO_PIN,
        Animation = PLAYING_ANIMATION_GPIO_PIN,
        Sound = PLAYING_SOUND_GPIO_PIN,
        ReceivingStreamFrames = RECEIVING_STREAM_FRAMES_GPIO_PIN,
        DMX = SENDING_DMX_GPIO_PIN,
        Heartbeat = HEARTBEAT_GPIO_PIN
    };

    class StatusLightEvent : public EventBase<StatusLightEvent> {
    public:
        using EventBase::EventBase;
        StatusLightEvent(uint64_t frameNumber, StatusLight light, bool on);
        void executeImpl();

    private:
        StatusLight light;
        bool on;
    };

}