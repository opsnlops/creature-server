#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <SDL.h>
#include <SDL_mixer.h>

#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/eventloop/events/types.h"
#include "server/eventloop/eventloop.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    extern const char* audioDevice;
    extern SDL_AudioSpec localAudioDeviceAudioSpec;
    extern std::shared_ptr<Configuration> config;
    extern std::shared_ptr<GPIO> gpioPins;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<ObservabilityManager> observability;

    /**
     * This event type plays a sound file, either locally via SDL or over RTSP/RTP
     * depending on the configured audio mode
     */
    MusicEvent::MusicEvent(framenum_t frameNumber, std::string filePath)
            : EventBase(frameNumber), filePath(std::move(filePath)) {}

    void MusicEvent::executeImpl() {

        debug("starting MusicEvent processing! filePath: {}", filePath);

        // Error checking
        if (filePath.empty()) {
            error("unable to play an empty file");
            return;
        }

        // Check if the file exists and is a regular file
        std::filesystem::path p(filePath);
        if (!std::filesystem::exists(p)) {
            error("File does not exist: {}", filePath);
            return;
        }
        if (!std::filesystem::is_regular_file(p)) {
            error("Not a regular file: {}", filePath);
            return;
        }

        // Check if the file is readable
        std::ifstream file(filePath);
        if (!file.good()) {
            error("File is not readable: {}", filePath);
            return;
        }

        // Create observability span for this music event
        auto span = observability->createOperationSpan("music_event.execute");
        span->setAttribute("file_path", filePath);

        // Handle different audio modes
        Configuration::AudioMode audioMode = config->getAudioMode();

        if (audioMode == Configuration::AudioMode::Local) {
            debug("Using local audio playback mode");
            span->setAttribute("audio_mode", "local");
            playLocalAudio(span);
        } else if (audioMode == Configuration::AudioMode::RTP) {
            debug("Using RTSP/RTP streaming mode");
            span->setAttribute("audio_mode", "rtsp_rtp");
            scheduleRtspAudio(span);
        }

        span->setSuccess();
    }

    void MusicEvent::playLocalAudio(std::shared_ptr<OperationSpan> parentSpan) {

        if(!parentSpan) {
            warn("No parent span provided for MusicEvent.playLocalAudio, creating a root span");
            parentSpan = observability->createOperationSpan("music_event.play_local_audio");
        }

        auto span = observability->createChildOperationSpan("music_event.play_local", parentSpan);
        span->setAttribute("file_path", filePath);

        // This is your existing SDL-based audio playback in a thread
        std::thread([filePath = this->filePath, span] {

            info("music playing thread running for file {}", filePath);
            Mix_Music *music;

            // Signal that we're playing music
            gpioPins->playingSound(true);

            // Initialize SDL_mixer for local audio device
            if (Mix_OpenAudioDevice(localAudioDeviceAudioSpec.freq,
                                    localAudioDeviceAudioSpec.format,
                                    localAudioDeviceAudioSpec.channels,
                                    SOUND_BUFFER_SIZE,
                                    audioDevice,
                                    1) < 0) {
                std::string errorMessage = fmt::format("Failed to open audio device: {}", Mix_GetError());
                error(errorMessage);
                span->setError(errorMessage);
                goto end;
            }

            // Play at full volume
            Mix_VolumeMusic(255);

            // Load the file
            music = Mix_LoadMUS(filePath.c_str());
            if (!music) {
                std::string errorMessage = fmt::format("Failed to load music file {}: {}", filePath, Mix_GetError());
                error(errorMessage);
                span->setError(errorMessage);
                goto end;
            }

            // Log the expected length
            debug("file is {0:.3f} seconds long", Mix_MusicDuration(music));
            span->setAttribute("duration_seconds", Mix_MusicDuration(music));

            // Play the music.
            if (Mix_PlayMusic(music, 1) == -1) {
                std::string errorMessage = fmt::format("Failed to play music: {}", Mix_GetError());
                error(errorMessage);
                span->setError(errorMessage);
                goto end;
            }

            // Wait for the music to finish
            while (Mix_PlayingMusic()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Clean up!
            Mix_FreeMusic(music);
            metrics->incrementSoundsPlayed();
            span->setSuccess();

            end:
            info("goodbye from the music thread! 👋🏻");
            gpioPins->playingSound(false);

        }).detach();
    }

    void MusicEvent::scheduleRtspAudio(std::shared_ptr<OperationSpan> parentSpan) {

        if(!parentSpan) {
            warn("No parent span provided for MusicEvent.scheduleRtspAudio, creating a root span");
            parentSpan = observability->createOperationSpan("music_event.schedule_rtsp_audio");
        }

        auto span = observability->createChildOperationSpan("music_event.schedule_rtsp", parentSpan);
        span->setAttribute("file_path", filePath);

        info("Loading audio file for RTSP/RTP streaming: {}", filePath);

        // Load the audio file and prepare for RTSP/RTP streaming
        auto audioBuffer = std::make_unique<rtp::AudioStreamBuffer>();
        if (!audioBuffer->loadFile(filePath, span)) {
            error("Failed to load audio file for RTSP/RTP streaming: {}", filePath);
            span->setError("Failed to load audio file");
            return;
        }

        size_t chunkCount = audioBuffer->getChunkCount();
        uint32_t duration = audioBuffer->getDurationMs();

        span->setAttribute("chunk_count", static_cast<int64_t>(chunkCount));
        span->setAttribute("duration_ms", duration);
        span->setAttribute("chunk_size_ms", RTP_FRAME_MS);

        info("Scheduling {} RTSP/RTP audio chunks over {}ms ({}ms per chunk)",
             chunkCount, duration, RTP_FRAME_MS);

        // Calculate frame interval for chunks (convert ms to frames)
        framenum_t framesPerChunk = RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;  // 5ms / 1ms = 5 frames
        framenum_t currentFrame = frameNumber + 10;  // Small delay to ensure proper ordering

        // Schedule all chunks in the event loop - this is the magic! 🐰
        for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            const auto* chunk = audioBuffer->getChunk(chunkIndex);
            if (!chunk) {
                warn("Failed to get chunk {} for RTSP/RTP scheduling", chunkIndex);
                continue;
            }

            // Create the L16 payload for this chunk (pure PCM data)
            auto payload = rtp::AudioStreamBuffer::createMultiChannelPayload(
                chunk, currentFrame, span);

            // Create an RTSP/RTP event for this chunk
            auto rtspEvent = std::make_shared<RtspAudioChunkEvent>(currentFrame);
            rtspEvent->setAudioPayload(std::move(payload));

            // Schedule it in the event loop
            eventLoop->scheduleEvent(rtspEvent);

            // Advance to the next chunk time
            currentFrame += framesPerChunk;
        }

        span->setAttribute("frames_scheduled", static_cast<int64_t>(chunkCount));
        span->setAttribute("total_duration_frames", static_cast<int64_t>(currentFrame - frameNumber));
        span->setSuccess();

        info("Successfully scheduled {} RTSP/RTP audio chunks for streaming", chunkCount);
        metrics->incrementSoundsPlayed();  // Count RTSP sounds too
    }




    // Fire up SDL
    int MusicEvent::initSDL() {

        debug("starting to bring up SDL");

        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
            error("Couldn't initialize SDL: {}", SDL_GetError());
            return 0;
        }

        debug("SDL init successful!");

        return 1;
    }

    // Locate the audio device
    int MusicEvent::locateAudioDevice() {

        debug("opening the audio device");

        localAudioDeviceAudioSpec = SDL_AudioSpec();
        localAudioDeviceAudioSpec.freq = (int)config->getSoundFrequency();
        localAudioDeviceAudioSpec.channels = config->getSoundChannels();
        localAudioDeviceAudioSpec.format = AUDIO_F32SYS;
        localAudioDeviceAudioSpec.samples = SOUND_BUFFER_SIZE;
        localAudioDeviceAudioSpec.callback = nullptr;
        localAudioDeviceAudioSpec.userdata = nullptr;

        // Get the name of the default
        audioDevice = SDL_GetAudioDeviceName(config->getSoundDevice(), 0);
        if (!audioDevice) {
            error("Failed to get audio device name: {}", SDL_GetError());
            return 0;
        }
        debug("Using audio device name: {}", audioDevice);

        return 1;
    }

    void MusicEvent::listAudioDevices() {

        int numDevices = SDL_GetNumAudioDevices(0);

        debug("Number of audio devices: {}", numDevices);

        for (int i = 0; i < numDevices; ++i) {
            const char* deviceName = SDL_GetAudioDeviceName(i, 0);
            if (deviceName) {
                debug(" Device: {}, Name: {}", i, deviceName);
            }
        }

    }
}