//
// music.cpp
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
#include "util/ObservabilityManager.h"


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

    // Use constants from config.h - 17 channels for 16 creatures + BGM

    // ───── Fixed SimpleLambdaEvent (use this one!) ───────────────────────────
    template <typename Tag>
    class SimpleLambdaEvent : public EventBase<SimpleLambdaEvent<Tag>> {
    public:
        using Fn = std::function<void()>;  // ← Simple void function

        SimpleLambdaEvent(framenum_t frameNum, Fn func)
            : EventBase<SimpleLambdaEvent<Tag>>(frameNum), fn_(std::move(func)) {}

        // Wraps void function in Result
        Result<framenum_t> executeImpl() {
            try {
                fn_();
                return Result<framenum_t>{this->frameNumber};  // Success!
            } catch (const std::exception& e) {
                return Result<framenum_t>{
                    ServerError(ServerError::InternalError,
                               std::string("SimpleLambdaEvent failed: ") + e.what())
                };
            } catch (...) {
                return Result<framenum_t>{
                    ServerError(ServerError::InternalError, "SimpleLambdaEvent failed with unknown error")
                };
            }
        }

    private:
        Fn fn_;
    };

// ───── MusicEvent impl ───────────────────────────────────────────────────
    MusicEvent::MusicEvent(const framenum_t frameNumber, std::string filePath)
        : EventBase(frameNumber), filePath(std::move(filePath)) {}

    Result<framenum_t> MusicEvent::executeImpl()
    {
        // Create an observability span if observability is available
        std::shared_ptr<OperationSpan> span;
        if (observability) {
            span = observability->createOperationSpan("music_event.execute");
            span->setAttribute("file_path", filePath);
        }

        // Make sure the file exists and is readable
        if (filePath.empty()) {
            std::string errorMessage = "MusicEvent: empty file path provided";
            error(errorMessage);
            if (span) span->setError(errorMessage);
            return Result<framenum_t>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        if (!std::filesystem::exists(filePath)) {
            std::string errorMessage = fmt::format("MusicEvent: file doesn't exist '{}'", filePath);
            error(errorMessage);
            if (span) span->setError(errorMessage);
            return Result<framenum_t>{ServerError(ServerError::NotFound, errorMessage)};
        }

        if (!std::filesystem::is_regular_file(filePath)) {
            std::string errorMessage = fmt::format("MusicEvent: not a regular file '{}'", filePath);
            error(errorMessage);
            if (span) span->setError(errorMessage);
            return Result<framenum_t>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        // Test readability
        if (std::ifstream test(filePath); !test.good()) {
            std::string errorMessage = fmt::format("MusicEvent: unreadable file '{}'", filePath);
            error(errorMessage);
            if (span) span->setError(errorMessage);
            return Result<framenum_t>{ServerError(ServerError::Forbidden, errorMessage)};
        }

        // Dispatch based on audio mode
        Result result = {this->frameNumber};  // Default to current frame number
        if (config->getAudioMode() == Configuration::AudioMode::RTP) {
            result = scheduleRtpAudio(span);
        } else {
            result = playLocalAudio(span);
        }

        if (result.isSuccess()) {
            if (span) span->setSuccess();
            debug("MusicEvent hopped successfully! 🐰🎵");
        } else {
            if (span) span->setError(result.getError()->getMessage());
            warn("MusicEvent stumbled: {}", result.getError()->getMessage());
        }

        return result;
    }

// ───── Local SDL playback ────────────────────────────────────────────────
    Result<framenum_t> MusicEvent::playLocalAudio(std::shared_ptr<OperationSpan> parentSpan)
    {
        std::shared_ptr<OperationSpan> span;
        if (observability) {
            if (!parentSpan && observability) {
                parentSpan = observability->createOperationSpan("music_event");
            }
            span = observability->createChildOperationSpan("music_event.play_local", parentSpan);
            span->setAttribute("file_path", filePath);
        }

        debug("Starting local audio playback for: {}", filePath);

        // Spawn the audio thread
        std::thread([filePath = this->filePath, span] {
            gpioPins->playingSound(true);

            // open audio device
            if (Mix_OpenAudioDevice(localAudioDeviceAudioSpec.freq,
                                    localAudioDeviceAudioSpec.format,
                                    localAudioDeviceAudioSpec.channels,
                                    SOUND_BUFFER_SIZE,
                                    audioDevice, 1) < 0)
            {
                const std::string errorMsg = fmt::format("Failed to open audio device: {}", Mix_GetError());
                error(errorMsg);
                if (span) span->setError(errorMsg);
                gpioPins->playingSound(false);
                return;
            }

            // load music
            Mix_Music* mus = Mix_LoadMUS(filePath.c_str());
            if (!mus) {
                const std::string errorMsg = fmt::format("Failed to load music: {}", Mix_GetError());
                error(errorMsg);
                if (span) span->setError(errorMsg);
                gpioPins->playingSound(false);
                return;
            }

            if (span) span->setAttribute("duration_seconds", Mix_MusicDuration(mus));
            debug("Music duration: {:.2f} seconds", Mix_MusicDuration(mus));

            if (Mix_PlayMusic(mus, 1) == -1) {
                const std::string errorMsg = fmt::format("Failed to play music: {}", Mix_GetError());
                error(errorMsg);
                if (span) span->setError(errorMsg);
                Mix_FreeMusic(mus);
                gpioPins->playingSound(false);
                return;
            }

            // Wait for playback to finish
            while (Mix_PlayingMusic()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Cleanup
            Mix_FreeMusic(mus);
            metrics->incrementSoundsPlayed();
            if (span) span->setSuccess();
            gpioPins->playingSound(false);

            debug("Local audio playback completed successfully! 🐰🎵");
        }).detach();

        // Return immediately - the music plays in the background
        return Result{this->frameNumber};
    }

// ───── RTP / Opus streaming with proper Result handling ──────────────────
    Result<framenum_t> MusicEvent::scheduleRtpAudio(std::shared_ptr<OperationSpan> parentSpan)
{
        std::shared_ptr<OperationSpan> span;
        if (observability) {
            if (!parentSpan) {
                parentSpan = observability->createOperationSpan("music_event");
            }
            span = observability->createChildOperationSpan("music_event.schedule_rtp", parentSpan);
        }

        // Validate RTP server availability
        if (!rtpServer || !rtpServer->isReady()) {
            std::string errorMsg = "RTP server not ready - can't hop the audio stream! 🐰";
            error(errorMsg);
            if (span) span->setError(errorMsg);
            return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
        }

        debug("RTP server is ready, preparing audio stream for: {}", filePath);

        // Capture the data we need by value so the MusicEvent can die
        const std::string localPath = filePath;
        const framenum_t startingFrame = eventLoop->getNextFrameNumber() + 1;
        if (span) span->setAttribute("original_frame_number", startingFrame);

    #if defined(__cpp_lib_jthread)
        std::jthread worker([span, localPath, startingFrame](std::stop_token st) {
    #else
        std::thread worker([span, localPath, startingFrame]() {
    #endif
            debug("RTP worker thread hopping into action! 🐰");

            // Heavy I/O – off the event‑loop thread!
            debug("Loading audio buffer from WAV file: {}", localPath);
            std::shared_ptr<OperationSpan> encodingSpan;
            if (observability && span) {
                encodingSpan = observability->createChildOperationSpan("music_event.encode_to_opus", span);
            }

            auto buffer = rtp::AudioStreamBuffer::loadFromWav(localPath, span);
            if (!buffer) {
                const auto msg = fmt::format("Failed to load audio buffer from '{}'", localPath);
                error(msg);
                if (encodingSpan) encodingSpan->setError(msg);
                if (span) span->setError(msg);
                return;
            }
            if (encodingSpan) encodingSpan->setSuccess();

            // Get current frame for scheduling (encoding took time!)
            framenum_t streamingStartFrame = eventLoop->getNextFrameNumber() + 2; // +2 to allow for reset event
            if (span) span->setAttribute("streaming_start_frame", streamingStartFrame);
            debug("Original frame: {}, streaming start frame: {}", startingFrame, streamingStartFrame);

            // *** NEW: Schedule encoder reset event one frame before streaming starts ***
            framenum_t resetFrame = streamingStartFrame - 1;
            auto resetEvent = std::make_shared<RtpEncoderResetEvent>(resetFrame, 4); // 4 silent frames
            eventLoop->scheduleEvent(resetEvent);
            debug("Scheduled RtpEncoderResetEvent for frame {} (one frame before streaming)", resetFrame);

            constexpr std::size_t kPrefill = 3;  // 30ms priming
            const std::size_t frames = buffer->frameCount();
            framenum_t cursor = streamingStartFrame;

            if (span) span->setAttribute("frames_total", static_cast<int64_t>(frames));
            debug("Scheduling {} frames for RTP streaming", frames);

            // Schedule all the audio frames as events
            for (std::size_t f = 0; f < frames
    #if defined(__cpp_lib_jthread)
                 && !st.stop_requested()
    #endif
                 ; ++f) {

                // Use SimpleLambdaEvent for the RTP send operations
                using Tag = struct RtpSendTag {};
                auto ev = std::make_shared<SimpleLambdaEvent<Tag>>(cursor, [buffer, f] {
                    // Send to all 17 RTP channels (16 creatures + 1 BGM) 🐰
                    for (int ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch) {
                        rtpServer->send(static_cast<uint8_t>(ch), buffer->frame(static_cast<uint8_t>(ch), f));
                    }
                    // Note: SimpleLambdaEvent will automatically wrap this in a successful Result
                });

                eventLoop->scheduleEvent(std::move(ev));

                // Calculate next frame timing
                cursor += (f + 1 < kPrefill) ? 1 : RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;
            }

            debug("Finished scheduling {} RTP audio frames", frames);
            metrics->incrementSoundsPlayed();
            if (span) span->setSuccess();
        });

        debug("Detaching RTP worker thread");
        worker.detach();

        // Return success immediately - the RTP streaming happens in the background
        return Result<framenum_t>{this->frameNumber};
    }

    // ───── SDL helpers (unchanged) ────────────────────────────────────────────
    int MusicEvent::initSDL()
    {
        debug("Initializing SDL for audio - getting ready to hop! 🐰");
        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
            error("SDL init failed: {}", SDL_GetError());
            return 0;
        }
        debug("SDL initialized successfully!");
        return 1;
    }

    int MusicEvent::locateAudioDevice()
    {
        debug("Locating audio device for local playback");

        localAudioDeviceAudioSpec = {};
        localAudioDeviceAudioSpec.freq = static_cast<int>(config->getSoundFrequency());
        localAudioDeviceAudioSpec.channels = config->getSoundChannels();
        localAudioDeviceAudioSpec.format = AUDIO_F32SYS;
        localAudioDeviceAudioSpec.samples = SOUND_BUFFER_SIZE;

        audioDevice = SDL_GetAudioDeviceName(config->getSoundDevice(), 0);
        if (!audioDevice) {
            error("SDL_GetAudioDeviceName failed: {}", SDL_GetError());
            return 0;
        }

        debug("Using audio device: {}", audioDevice);
        return 1;
    }

    void MusicEvent::listAudioDevices()
    {
        int n = SDL_GetNumAudioDevices(0);
        debug("SDL reports {} audio devices available:", n);
        for (int i = 0; i < n; ++i) {
            const char* deviceName = SDL_GetAudioDeviceName(i, 0);
            debug("  [{}] {}", i, deviceName ? deviceName : "Unknown");
        }
    }

} // namespace creatures
