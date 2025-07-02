//
// music.cpp  –  Opus-only version for 10 ms multicast
//
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <SDL.h>
#include <SDL_mixer.h>
#include <spdlog/spdlog.h>

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/eventloop/events/types.h"
#include "server/eventloop/eventloop.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/MultiOpusRtpServer.h"

#include "server/namespace-stuffs.h"

namespace creatures {

// ─────────── singletons from main.cpp ───────────
extern const char*                        audioDevice;
extern SDL_AudioSpec                      localAudioDeviceAudioSpec;
extern std::shared_ptr<Configuration>     config;
extern std::shared_ptr<GPIO>              gpioPins;
extern std::shared_ptr<SystemCounters>    metrics;
extern std::shared_ptr<EventLoop>         eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;

// ─────────── utility: generic lambda event ───────
    template <typename UniqueTag>
    class LambdaEvent : public EventBase<LambdaEvent<UniqueTag>> {
    public:
        using Fn = std::function<void()>;
        LambdaEvent(framenum_t fn, Fn fnc)
            : EventBase<LambdaEvent<UniqueTag>>(fn), fn_(std::move(fnc)) {}

        void executeImpl() { fn_(); }   // ← no 'override', and it's public
    private:
        Fn fn_;
    };

// ─────────── MusicEvent implementation ───────────
    MusicEvent::MusicEvent(framenum_t fn, std::string path)
        : EventBase(fn), filePath(std::move(path)) {}

    void MusicEvent::executeImpl()
    {
        // quick sanity on the file
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

        switch (config->getAudioMode()) {
            case Configuration::AudioMode::Local:
                playLocalAudio(span);
                break;
            case Configuration::AudioMode::RTP:
                scheduleRtpAudio(span);
                break;
        }
        span->setSuccess();
    }

    // ─────────── LOCAL (SDL)  ────────────────────────
    void MusicEvent::playLocalAudio(std::shared_ptr<OperationSpan> parentSpan)
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

    // ─────────── RTP / Opus  ─────────────────────────
    void MusicEvent::scheduleRtpAudio(std::shared_ptr<OperationSpan> parentSpan)
    {
        if (!parentSpan)
            parentSpan = observability->createOperationSpan("music_event.schedule_rtp_audio");
        auto span = observability->createChildOperationSpan("music_event.schedule_rtp", parentSpan);

        if (!rtpServer || !rtpServer->isReady()) {
            span->setError("rtp_server_not_ready");
            error("Opus RTP server not ready – aborting stream");
            return;
        }

        auto buffer = rtp::AudioStreamBuffer::loadFromWav(filePath, span);
        if (!buffer) { span->setError("buffer_load_failed"); return; }

        const std::size_t frames = buffer->frameCount();
        constexpr std::size_t kPrefill = 3;           // first 30 ms @ 1 ms pacing
        framenum_t cursor = frameNumber + 1;          // 1 ms after this event

        span->setAttribute("frames_total", static_cast<int64_t>(frames));

        for (std::size_t f = 0; f < frames; ++f) {

            using Tag = struct {}; // unique tag to satisfy EventBase template
            auto ev = std::make_shared<LambdaEvent<Tag>>(cursor, [buf = buffer, f] {
                for (uint8_t ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch)
                    rtpServer->send(ch, buf->frame(ch, f));
            });

            eventLoop->scheduleEvent(ev);

            cursor += (f + 1 < kPrefill)
                      ? 1                                         // priming burst
                      : RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;      // normal 10 ms
        }

        metrics->incrementSoundsPlayed();
        span->setSuccess();
    }

    // ─────────── SDL helpers (unchanged) ─────────────
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