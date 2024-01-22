
#include <iostream>
#include <string>

#include <SDL.h>

#include <argparse/argparse.hpp>
#include <fmt/format.h>
#include <utility>

#include "Configuration.h"
#include "CommandLine.h"

#include "Version.h"

#include "server/config.h"
#include "server/namespace-stuffs.h"
#include "util/environment.h"


/*
 * This is using argparse:
 *   https://github.com/p-ranav/argparse
 */

namespace creatures {

    std::shared_ptr<Configuration> CommandLine::parseCommandLine(int argc, char **argv) {

        auto config = std::make_shared<Configuration>();

        argparse::ArgumentParser program("creature-server", getVersion());


        program.add_argument("-g", "--use-gpio")
                .help("use the GPIO pins? (RPI only!)")
                .default_value(environmentToInt(USE_GPIO_ENV, DEFAULT_USE_GPIO) == 1)
                .implicit_value(true);

        program.add_argument("-d", "--mongodb-uri")
                .help("MongoDB URI to use")
                .default_value(environmentToString(DB_URI_ENV, DEFAULT_DB_URI))
                .nargs(1);

        program.add_argument("-s", "--sound-device")
                .help("sound device to use")
                .default_value(environmentToInt(SOUND_DEVICE_NUMBER_ENV, DEFAULT_SOUND_DEVICE_NUMBER))
                .nargs(1)
                .scan<'i', int>();

        program.add_argument("-c", "--sound-channels")
                .help("number of sound channels to use")
                .default_value(environmentToInt(SOUND_CHANNELS_ENV, DEFAULT_SOUND_CHANNELS))
                .nargs(1)
                .scan<'i', int>();

        program.add_argument("-f", "--sound-frequency")
                .help("frequency of the sound channels in Hz")
                .default_value(environmentToInt(SOUND_FREQUENCY_ENV, DEFAULT_SOUND_FREQUENCY))
                .nargs(1)
                .scan<'i', int>();

        program.add_argument("-l", "--sounds-location")
                .help("location on the file system of the sound files")
                .default_value(environmentToString(SOUND_FILE_LOCATION_ENV, DEFAULT_SOUND_FILE_LOCATION))
                .nargs(1);

        program.add_argument("--list-sound-devices")
                .help("list available sound devices and exit")
                .default_value(false)
                .implicit_value(true);


        program.add_description("Creature Server for April's Creature Workshop! 🐰\n\n"
                                "This application is the heart of my creature magic. It contains a gRPC-based server\n"
                                "as well as the event loop that schedules events to happen in real time.");
        program.add_epilog("There are environment variables, too, if you'd rather configure in a Docker-friendly\n"
                           "sort of way.\n\n"
                           "This is version " + getVersion() + ".\n\n"
                           "🦜 Bawk!");

        try {
            program.parse_args(argc, argv);
        }
        catch (const std::exception &err) {

            critical(err.what());

            std::cerr << "\n" << program;
            std::exit(1);
        }

        debug("Parsing the command line options");

        // Do the sound listing first to avoid weird debug messages
        if(program.get<bool>("--list-sound-devices")) {
            listSoundDevices();
            std::exit(0);
        }

        auto useGPIO = program.get<bool>("-g");
        debug("read use GPIO {} from command line", useGPIO);
        if(useGPIO) {
            config->setUseGPIO(useGPIO);
            debug("set our use GPIO to {}", useGPIO);
        }

        auto mongoURI = program.get<std::string>("-d");
        debug("read mongo URI {} from command line", mongoURI);
        if(!mongoURI.empty()) {
            config->setMongoURI(mongoURI);
            debug("set our mongo URI to {}", mongoURI);
        }

        auto soundDevice = program.get<int>("-s");
        debug("read sound device {} from command line", soundDevice);
        if(soundDevice >= 0) {
            config->setSoundDevice(soundDevice);
            debug("set our sound device to {}", soundDevice);
        }

        auto soundChannels = program.get<int>("-c");
        debug("read sound channels {} from command line", soundChannels);
        if(soundChannels > 0) {
            config->setSoundChannels(soundChannels);
            debug("set our sound channels to {}", soundChannels);
        }

        auto soundFrequency = program.get<int>("-f");
        debug("read sound frequency {} from command line", soundFrequency);
        if(soundFrequency > 0) {
            config->setSoundFrequency(soundFrequency);
            debug("set our sound frequency to {}", soundFrequency);
        }

        auto soundsLocation = program.get<std::string>("-l");
        debug("read sounds location {} from command line", soundsLocation);
        if(!soundsLocation.empty()) {
            config->setSoundFileLocation(soundsLocation);
            debug("set our sound file location to {}", soundsLocation);
        }



        return config;
    }

    void CommandLine::listSoundDevices() {

        if(SDL_Init(SDL_INIT_AUDIO) < 0) {
            std::string errorMessage = fmt::format("Couldn't initialize SDL: {}\n", SDL_GetError());
            fprintf(stderr, "%s", errorMessage.c_str());
            return;
        }

        int numDevices = SDL_GetNumAudioDevices(0);
        printf("Number of audio devices found: %d\n", numDevices);

        for (int i = 0; i < numDevices; ++i) {
            const char* deviceName = SDL_GetAudioDeviceName(i, 0);
            if (deviceName) {
                printf(" Device: %d, Name: %s\n", i, deviceName);
            }
        }
    }

    std::string CommandLine::getVersion() {
        return fmt::format("{}.{}.{}",
                    CREATURE_SERVER_VERSION_MAJOR,
                    CREATURE_SERVER_VERSION_MINOR,
                    CREATURE_SERVER_VERSION_PATCH);
    }
}