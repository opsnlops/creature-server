#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <SDL_mixer.h>

#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "util/environment.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    extern const char* audioDevice;
    extern SDL_AudioSpec audioSpec;
    extern std::shared_ptr<Configuration> config;
    extern std::shared_ptr<GPIO> gpioPins;
    extern std::shared_ptr<SystemCounters> metrics;

    /**
     * This event type plays a sound file on a thread in the background
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

        // Looks good, start the thread
        std::thread([filePath = this->filePath] {

            info("music playing thread running for file {}", filePath);
            Mix_Music *music;

            // Signal that we're playing music
            gpioPins->playingSound(true);

            // Only one file can be playing at once...
            //std::lock_guard<std::mutex> lock(sdl_mutex);

            // Initialize SDL_mixer for stereo sound (set channels to 6 for 5.1)
            if (Mix_OpenAudioDevice(audioSpec.freq, audioSpec.format, audioSpec.channels, SOUND_BUFFER_SIZE, audioDevice, 1) < 0) {
                error("Failed to initialize SDL_mixer: {}", Mix_GetError());
                goto end;
            }

            // Play at full volume
            Mix_VolumeMusic(255);

            // Load the file
            music = Mix_LoadMUS(filePath.c_str());
            if (!music) {
                error("Failed to load music: {}", Mix_GetError());
                goto end;
            }

            // Log the expected length
            debug("file is {0:.3f} seconds long", Mix_MusicDuration(music));

            // Play the music.
            if (Mix_PlayMusic(music, 1) == -1) {
                error("Failed to play music: {}", Mix_GetError());
                goto end;
            }

            // Wait for the music to finish
            while (Mix_PlayingMusic()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Clean up!
            Mix_FreeMusic(music);

            metrics->incrementSoundsPlayed();

            end:
            info("goodbye from the music thread! 👋🏻");
            gpioPins->playingSound(false);

        }).detach();
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

        audioSpec = SDL_AudioSpec();
        audioSpec.freq = (int)config->getSoundFrequency();
        audioSpec.channels = config->getSoundChannels();
        audioSpec.format = AUDIO_F32SYS;
        audioSpec.samples = SOUND_BUFFER_SIZE;
        audioSpec.callback = nullptr;
        audioSpec.userdata = nullptr;

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