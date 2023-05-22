#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>


#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/eventloop/events/types.h"

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;


namespace creatures {

    extern const char* audioDevice;
    extern SDL_AudioSpec audioSpec;


    /**
     * This event type plays a sound file on a thread in the background
     */
    MusicEvent::MusicEvent(int frameNumber, std::string filePath)
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

            // Only one file can be playing at once...
            //std::lock_guard<std::mutex> lock(sdl_mutex);

            // Initialize SDL_mixer for stereo sound (set channels to 6 for 5.1)
            if (Mix_OpenAudioDevice(audioSpec.freq, audioSpec.format, audioSpec.channels, SOUND_BUFFER_SIZE, audioDevice, 1) < 0) {
                error("Failed to initialize SDL_mixer: {}", Mix_GetError());
                return;
            }

            // Play at full volume
            Mix_VolumeMusic(255);

            // Load the file
            Mix_Music *music = Mix_LoadMUS(filePath.c_str());
            if (!music) {
                error("Failed to load music: {}", Mix_GetError());
                return;
            }

            // Log the expected length
            debug("file is {0:.3f} seconds long", Mix_MusicDuration(music));

            // Play the music.
            if (Mix_PlayMusic(music, 1) == -1) {
                error("Failed to play music: {}", Mix_GetError());
                return;
            }

            // Wait for the music to finish
            while (Mix_PlayingMusic()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Clean up!
            Mix_FreeMusic(music);

            info("goodbye from the music thread! 👋🏻");

        }).detach();
    }


    /**
     * Returns the directory of where we're expecting to find sound files
     *
     * @return The value of SOUND_FILE_LOCATION_ENV from config.h, or the default.
     */
    std::string MusicEvent::getSoundFileLocation() {
        const char *val = std::getenv(SOUND_FILE_LOCATION_ENV);
        if (val == nullptr) {
            return DEFAULT_SOUND_FILE_LOCATION;
        } else {
            return val;
        }
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

        int deviceNumber = environmentToInt(SOUND_DEVICE_NUMBER_ENV, DEFAULT_SOUND_DEVICE_NUMBER);
        int frequency = environmentToInt(SOUND_FREQUENCY_ENV, DEFAULT_SOUND_FREQUENCY);
        int channels = environmentToInt(SOUND_CHANNELS_ENV, DEFAULT_SOUND_CHANNELS);

        audioSpec = SDL_AudioSpec();
        audioSpec.freq = frequency;
        audioSpec.format = MIX_DEFAULT_FORMAT;
        audioSpec.channels = channels;
        audioSpec.samples = SOUND_BUFFER_SIZE;
        audioSpec.callback = nullptr;
        audioSpec.userdata = nullptr;

        // Get the name of the default
        audioDevice = SDL_GetAudioDeviceName(deviceNumber, 0);
        if (!audioDevice) {
            error("Failed to get audio device name: {}", SDL_GetError());
            return 0;
        }
        debug("Using audio device name: {}", audioDevice);

        return 1;
    }

    int MusicEvent::environmentToInt(const char *variable, const char *defaultValue) {
        return environmentToInt(variable,std::stoi(std::string(defaultValue)));
    }

    int MusicEvent::environmentToInt(const char* variable, int defaultValue) {
        trace("converting {} to an int from the environment (default is {})", variable, defaultValue);

        int value;
        const char* valueString = std::getenv(variable);
        if(valueString != nullptr) {

            try {

                value = std::stoi(std::string(valueString));
                trace("environment var {} is {}", variable, value);
                return value;

            } catch (std::invalid_argument& e) {
                error("{} is not an int?", variable);
                return defaultValue;
            } catch (std::out_of_range& e) {
                error("{} is out of range", variable);
                return defaultValue;
            }
        }
        else
        {
            trace("using the default of {}", defaultValue);
            return defaultValue;
        }
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