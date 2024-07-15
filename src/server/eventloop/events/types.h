
#pragma once

#include <cstdlib>
#include <vector>

#include "model/CacheInvalidation.h"
#include "model/PlaylistStatus.h"
#include "server/config.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    class TickEvent : public EventBase<TickEvent> {
    public:
        using EventBase::EventBase;
        void executeImpl();

        virtual ~TickEvent() = default;
    };

    class CounterSendEvent : public EventBase<CounterSendEvent> {
    public:
        using EventBase::EventBase;
        void executeImpl();

        virtual ~CounterSendEvent() = default;
    };

    class DMXEvent : public EventBase<DMXEvent> {
    public:
        using EventBase::EventBase;
        virtual ~DMXEvent() = default;
        void executeImpl();

        universe_t universe;
        uint32_t channelOffset;

        // Used every time to send data
        std::vector<uint8_t> data;
    };

    class MusicEvent : public EventBase<MusicEvent> {
    public:
        using EventBase::EventBase;
        MusicEvent(framenum_t frameNumber, std::string filePath);
        virtual ~MusicEvent() = default;
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
        PlaylistEvent(framenum_t frameNumber, universe_t universe);
        virtual ~PlaylistEvent() = default;
        void executeImpl();

    private:
        universe_t activeUniverse;
        static void sendEmptyPlaylistUpdate(universe_t universe);
        static void sendPlaylistUpdate(const PlaylistStatus &playlistStatus);
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
        StatusLightEvent(framenum_t frameNumber, StatusLight light, bool on);
        virtual ~StatusLightEvent() = default;
        void executeImpl();

    private:
        StatusLight light;
        bool on;
    };


    class CacheInvalidateEvent : public EventBase<CacheInvalidateEvent> {
    public:
        using EventBase::EventBase;
        CacheInvalidateEvent(framenum_t frameNumber, CacheType cacheType);
        virtual ~CacheInvalidateEvent() = default;
        void executeImpl();

    private:
        CacheType cacheType;
    };

}