//
// music.cpp – Opus-only server-side file streamer with RTP marker
//            (C++20 rev: heavy work happens on a std::jthread 🚀)
//
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>          // std::jthread lives here
#include <stop_token>      // std::stop_token (libc++ 15+)
#include <vector>

#include <SDL.h>
#include <SDL_mixer.h>
#include <spdlog/spdlog.h>

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "server/namespace-stuffs.h"

namespace creatures {

// ───── singletons from main.cpp ──────────────────────────────────────────
extern const char*                        audioDevice;
extern SDL_AudioSpec                      localAudioDeviceAudioSpec;
extern std::shared_ptr<Configuration>     config;
extern std::shared_ptr<GPIO>              gpioPins;
extern std::shared_ptr<SystemCounters>    metrics;
extern std::shared_ptr<EventLoop>         eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;

// ───── small helper: generic lambda event ────────────────────────────────
template <typename Tag>
class LambdaEvent : public EventBase<LambdaEvent<Tag>> {
public:
    using Fn = std::function<void()>;
    LambdaEvent(framenum_t fn, Fn fnc)
        : EventBase<LambdaEvent<Tag>>(fn), fn_(std::move(fnc)) {}
    void executeImpl() { fn_(); }
private:
    Fn fn_;
};

// ───── MusicEvent impl ───────────────────────────────────────────────────
MusicEvent::MusicEvent(const framenum_t frameNumber, std::string filePath)
    : EventBase(frameNumber), filePath(std::move(filePath)) {}

void MusicEvent::executeImpl()
{
    if (filePath.empty() || !std::filesystem::is_regular_file(filePath)) {
        error("MusicEvent: invalid file '{}'", filePath);
        return;
    }
    std::ifstream test(filePath);
    if (!test.good()) {
        error("MusicEvent: unreadable file '{}'", filePath);
        return;
    }

    auto span = observability->createOperationSpan("music_event.execute");
    span->setAttribute("file_path", filePath);

    if (config->getAudioMode() == Configuration::AudioMode::RTP)
        scheduleRtpAudio(span);
    else
        playLocalAudio(span);

    span->setSuccess();
}

// ───── Local SDL playback (unchanged) ────────────────────────────────────
void MusicEvent::playLocalAudio([[maybe_unused]] std::shared_ptr<OperationSpan> parentSpan)
{
    if (!parentSpan)
        parentSpan = observability->createOperationSpan("music_event.play_local_audio");

    auto span = observability->createChildOperationSpan("music_event.play_local", parentSpan);
    span->setAttribute("file_path", filePath);

    std::thread([filePath = this->filePath, span] {
        gpioPins->playingSound(true);

        // open audio device
        if (Mix_OpenAudioDevice(localAudioDeviceAudioSpec.freq,
                                localAudioDeviceAudioSpec.format,
                                localAudioDeviceAudioSpec.channels,
                                SOUND_BUFFER_SIZE,
                                audioDevice, 1) < 0)
        {
            span->setError(Mix_GetError());
            gpioPins->playingSound(false);
            return;
        }

        // load music
        Mix_Music* mus = Mix_LoadMUS(filePath.c_str());
        if (!mus) {
            span->setError(Mix_GetError());
            gpioPins->playingSound(false);
            return;
        }

        span->setAttribute("duration_seconds", Mix_MusicDuration(mus));

        if (Mix_PlayMusic(mus, 1) == -1) {
            span->setError(Mix_GetError());
            Mix_FreeMusic(mus);
            gpioPins->playingSound(false);
            return;
        }

        while (Mix_PlayingMusic())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        Mix_FreeMusic(mus);
        metrics->incrementSoundsPlayed();
        span->setSuccess();
        gpioPins->playingSound(false);
    }).detach();
}

// ───── RTP / Opus streaming – now fires a background jthread ─────────────
void MusicEvent::scheduleRtpAudio(std::shared_ptr<OperationSpan> parentSpan)
{
    if (!parentSpan)
        parentSpan = observability->createOperationSpan("music_event.schedule_rtp_audio");

    //---------------------------------------------------------------------
    // Capture the data we need *by value* so the MusicEvent object can die.
    //---------------------------------------------------------------------
    const std::string localPath  = filePath;

    // Ask the loop for the next safe frame *before* we spawn the thread.
    const framenum_t startingFrame  = eventLoop->getNextFrameNumber() + 1;
    parentSpan->setAttribute("original_frame_number", startingFrame);

#if defined(__cpp_lib_jthread)
    std::jthread worker([parentSpan, localPath, startingFrame](std::stop_token st) {
#else
    std::thread  worker([parentSpan, localPath, startingFrame]() {
#endif
        //-----------------------------------------------------------------
        auto span = observability->createChildOperationSpan("music_event.schedule_rtp", parentSpan);

        if (!rtpServer || !rtpServer->isReady()) {
            span->setError("rtp_server_not_ready");
            error("Opus RTP server not ready – aborting stream");
            return;
        }
        debug("rtpServer is ready, proceeding with RTP audio stream");

        // 🐇 Heavy I/O – off the event‑loop thread!
        debug("loading buffer from WAV file: {}", localPath);
        const auto encodingSpan = observability->createChildOperationSpan("music_event.encode_to_opus", span);
        auto buffer = rtp::AudioStreamBuffer::loadFromWav(localPath, span);
        if (!buffer) {
            const auto msg = fmt::format("Failed to load audio buffer from '{}'", localPath);
            error(msg);
            encodingSpan->setError(msg);
            return;
        }
        encodingSpan->setSuccess();

        // Since encoding takes a lot of time, we need to see where we are now. We can use that as the starting
        // point for the RTP stream. This is the frame number that will be used to schedule the first event.
        framenum_t streamingStartFrame = eventLoop->getNextFrameNumber() + 1;
        parentSpan->setAttribute("streaming_start_frame", streamingStartFrame);
        debug("Original music event frame number: {}, streaming start frame: {}",
              startingFrame, streamingStartFrame);

        constexpr std::size_t kPrefill = 3;  // 30ms priming
        const std::size_t     frames   = buffer->frameCount();
        framenum_t            cursor   = streamingStartFrame;

        span->setAttribute("frames_total", static_cast<int64_t>(frames));

        debug("scheduling {} frames for RTP streaming", frames);
        for (std::size_t f = 0; f < frames
#if defined(__cpp_lib_jthread)
             && !st.stop_requested()
#endif
             ; ++f) {
            using Tag = struct {};
            auto ev = std::make_shared<LambdaEvent<Tag>>(cursor, [buf = buffer, f] {
                for (uint8_t ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch)
                    rtpServer->send(ch, buf->frame(ch, f));
            });

            eventLoop->scheduleEvent(std::move(ev));
            cursor += (f + 1 < kPrefill) ? 1
                                         : RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;
        }
        debug("finished scheduling RTP audio frames");

        metrics->incrementSoundsPlayed();
        span->setSuccess();
    });

    debug("detaching worker");
    worker.detach();
    debug("worker detached");
}

    // ───── SDL helpers ───────────────────────────────────────────
    int MusicEvent::initSDL()
    {
        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
            error("SDL init failed: {}", SDL_GetError());
            return 0;
        }
        return 1;
    }

    int MusicEvent::locateAudioDevice()
    {
        localAudioDeviceAudioSpec = {};
        localAudioDeviceAudioSpec.freq     = static_cast<int>(config->getSoundFrequency());
        localAudioDeviceAudioSpec.channels = config->getSoundChannels();
        localAudioDeviceAudioSpec.format   = AUDIO_F32SYS;
        localAudioDeviceAudioSpec.samples  = SOUND_BUFFER_SIZE;

        audioDevice = SDL_GetAudioDeviceName(config->getSoundDevice(), 0);
        if (!audioDevice) { error("SDL_GetAudioDeviceName: {}", SDL_GetError()); return 0; }
        return 1;
    }

    void MusicEvent::listAudioDevices()
    {
        int n = SDL_GetNumAudioDevices(0);
        debug("SDL reports {} audio devices", n);
        for (int i = 0; i < n; ++i)
            debug("  [{}] {}", i, SDL_GetAudioDeviceName(i, 0));
    }

} // namespace creatures
