//
// types.h
//

#pragma once

#include <cstdlib>
#include <vector>

#include "model/CacheInvalidation.h"
#include "model/PlaylistStatus.h"
#include "server/config.h"
#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/rtp/AudioChunk.h"
#include "server/rtp/AudioStreamBuffer.h"

#include "server/namespace-stuffs.h"

namespace creatures {

// Forward declaration for PlaybackSession
class PlaybackSession;

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

    MusicEvent(framenum_t frameNumber_, std::string filePath_);

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

    PlaylistEvent(framenum_t frameNumber_, universe_t universe_);

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

    StatusLightEvent(framenum_t frameNumber_, StatusLight light_, bool on_);

    virtual ~StatusLightEvent() = default;

    Result<framenum_t> executeImpl();

  private:
    StatusLight light;
    bool on;
};

class CacheInvalidateEvent : public EventBase<CacheInvalidateEvent> {
  public:
    using EventBase::EventBase;

    CacheInvalidateEvent(framenum_t frameNumber_, CacheType cacheType_);

    virtual ~CacheInvalidateEvent() = default;

    Result<framenum_t> executeImpl();

  private:
    CacheType cacheType;
};

class RtpEncoderResetEvent : public EventBase<RtpEncoderResetEvent> {
  public:
    using EventBase::EventBase;

    // Constructor with silent frame count parameter
    RtpEncoderResetEvent(framenum_t frameNumber_, uint8_t silentFrameCount_ = 4);

    virtual ~RtpEncoderResetEvent() = default;

    Result<framenum_t> executeImpl();

  private:
    uint8_t silentFrameCount_{4}; // Default to 4 silent frames (80ms of priming)
};

/**
 * PlaybackRunnerEvent - Cooperative playback event that executes one "slice" of animation
 *
 * This event replaces the legacy bulk-scheduling approach where thousands of DMXEvent
 * and audio events were scheduled upfront. Instead, the PlaybackRunnerEvent:
 *
 * 1. Checks if the session has been cancelled
 * 2. If cancelled: performs teardown (DMX blackout, status light off) and exits
 * 3. If not cancelled:
 *    - Emits DMX payloads for the current frame
 *    - Dispatches audio chunks whose timestamp has arrived
 *    - Advances playback cursors
 *    - Schedules the next runner event for the smallest upcoming timestamp
 *
 * This keeps the event queue shallow - only the runner plus immediate DMX/audio events.
 * Allows instant cancellation by simply setting the session's cancelled flag.
 */
class PlaybackRunnerEvent : public EventBase<PlaybackRunnerEvent> {
  public:
    /**
     * Create a new playback runner event
     *
     * @param frameNumber Frame when this runner should execute
     * @param session The playback session to manage
     */
    PlaybackRunnerEvent(framenum_t frameNumber, std::shared_ptr<PlaybackSession> session);

    virtual ~PlaybackRunnerEvent() = default;

    /**
     * Execute one slice of the animation playback
     *
     * Checks cancellation, emits current frame's DMX/audio, schedules next runner.
     *
     * @return Last frame number processed, or error
     */
    Result<framenum_t> executeImpl();

  private:
    std::shared_ptr<PlaybackSession> session_;

    /**
     * Perform cleanup when playback ends (cancellation or natural completion)
     *
     * Sends DMX blackout, turns off status light, stops audio
     */
    void performTeardown();

    /**
     * Emit DMX frames for all tracks at the current frame
     *
     * @return Success or error
     */
    Result<framenum_t> emitDmxFrames();

    /**
     * Check if all animation tracks have finished
     *
     * @return true if all tracks complete
     */
    [[nodiscard]] bool areAllTracksFinished() const;

    /**
     * Calculate the next frame number when runner should execute
     *
     * @return Next frame number
     */
    [[nodiscard]] framenum_t calculateNextFrameNumber() const;
};

} // namespace creatures