//
// types.h
//

#pragma once

#include <cstdlib>
#include <vector>

#include "model/CacheInvalidation.h"
#include "model/PlaylistStatus.h"
#include "server/config.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"
#include "server/rtp/AudioChunk.h"
#include "server/rtp/AudioStreamBuffer.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    class TickEvent : public EventBase<TickEvent> {
    public:
        using EventBase::EventBase;

        Result<framenum_t> executeImpl();

        virtual ~TickEvent() = default;
    };

    class CounterSendEvent : public EventBase<CounterSendEvent> {
    public:
        using EventBase::EventBase;

        Result<framenum_t> executeImpl();

        virtual ~CounterSendEvent() = default;
    };

    class DMXEvent : public EventBase<DMXEvent> {
    public:
        using EventBase::EventBase;

        virtual ~DMXEvent() = default;

        Result<framenum_t> executeImpl();

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

        Result<framenum_t> executeImpl();
        static int initSDL();
        static int locateAudioDevice();
        static void listAudioDevices();

    private:
        std::string filePath;
        std::mutex sdl_mutex;

        /**
         * Play audio locally through SDL (traditional mode)
         * @param parentSpan Optional observability span for tracing
         */
        Result<framenum_t> playLocalAudio(std::shared_ptr<class OperationSpan> parentSpan = nullptr);

        /**
         * Schedule RTP audio chunks in the event loop (streaming mode)
         * @param parentSpan Optional observability span for tracing
         */
        Result<framenum_t> scheduleRtpAudio(std::shared_ptr<class OperationSpan> parentSpan = nullptr);
    };

    class PlaylistEvent : public EventBase<PlaylistEvent> {
    public:
        using EventBase::EventBase;

        PlaylistEvent(framenum_t frameNumber, universe_t universe);

        virtual ~PlaylistEvent() = default;

        Result<framenum_t> executeImpl();

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

        Result<framenum_t> executeImpl();

    private:
        StatusLight light;
        bool on;
    };


    class CacheInvalidateEvent : public EventBase<CacheInvalidateEvent> {
    public:
        using EventBase::EventBase;

        CacheInvalidateEvent(framenum_t frameNumber, CacheType cacheType);

        virtual ~CacheInvalidateEvent() = default;

        Result<framenum_t> executeImpl();

    private:
        CacheType cacheType;
    };

    class RtpEncoderResetEvent : public EventBase<RtpEncoderResetEvent> {
    public:
        using EventBase::EventBase;

        // Constructor with silent frame count parameter
        RtpEncoderResetEvent(framenum_t frameNumber, uint8_t silentFrameCount = 4);

        virtual ~RtpEncoderResetEvent() = default;

        Result<framenum_t> executeImpl();

    private:
        uint8_t silentFrameCount_{4};  // Default to 4 silent frames (80ms of priming)
    };

}