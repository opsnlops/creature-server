#include <arpa/inet.h>
#include <cstdio>
#include <ifaddrs.h>
#include <iostream>
#include <net/if.h>
#include <netinet/in.h>
#include <string>
#include <system_error>
#include <utility>

#include <SDL.h>

#include <argparse/argparse.hpp>
#include <fmt/format.h>

#include "CommandLine.h"
#include "Configuration.h"

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

    program.add_argument("-n", "--network-device")
        .help("network device to use")
        .default_value(environmentToString(NETWORK_DEVICE_NAME_ENV, DEFAULT_NETWORK_DEVICE_NAME))
        .nargs(1);

    program.add_argument("-v", "--voice-api-key")
        .help("ElevenLabs API key")
        .default_value(environmentToString(VOICE_API_KEY_ENV, DEFAULT_VOICE_API_KEY))
        .nargs(1);

    program.add_argument("-h", "--honeycomb-api-key")
        .help("Honeycomb API key")
        .default_value(environmentToString(HONEYCOMB_API_KEY_ENV, DEFAULT_HONEYCOMB_API_KEY))
        .nargs(1);

    program.add_argument("--rtp-fragment")
        .help("enable RTP packet fragmentation for standard MTU networks (WiFi, etc.)")
        .default_value(environmentToInt(RTP_FRAGMENT_PACKETS_ENV, DEFAULT_RTP_FRAGMENT_PACKETS) == 1)
        .implicit_value(true);

    auto &oneShots = program.add_mutually_exclusive_group();
    oneShots.add_argument("--list-sound-devices")
        .help("list available sound devices and exit")
        .default_value(false)
        .implicit_value(true);

    oneShots.add_argument("--list-network-devices")
        .help("list available network devices and exit")
        .default_value(false)
        .implicit_value(true);

    /*
     * We can only use one of these two audio modes at a time, so we use a mutually exclusive group.
     */
    auto &audioMode = program.add_mutually_exclusive_group();
    audioMode.add_argument("--local-audio")
        .help("use local audio playback (default)")
        .default_value(true)
        .implicit_value(true);

    audioMode.add_argument("--rtp-audio").help("use RTP audio streaming").default_value(false).implicit_value(true);

    program.add_description("Creature Server for April's Creature Workshop! 🐰\n\n"
                            "This application is the heart of my creature magic. It contains a websocket-based\n"
                            "server as well as the event loop that schedules events to happen in real time.");
    program.add_epilog("There are environment variables, too, if you'd rather configure in a Docker-friendly\n"
                       "sort of way.\n\n"
                       "This is version " +
                       getVersion() +
                       ".\n\n"
                       "🦜 Bawk!");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception &err) {

        critical(err.what());

        std::cerr << "\n" << program;
        std::exit(1);
    }

    debug("Parsing the command line options");

    // Do the sound and network listing(s) first to avoid weird debug messages
    if (program.get<bool>("--list-sound-devices")) {
        listSoundDevices();
        std::exit(0);
    }

    if (program.get<bool>("--list-network-devices")) {
        listNetworkDevices();
        std::exit(0);
    }

    // What audio mode are we using?
    if (program.get<bool>("--rtp-audio")) {
        config->setAudioMode(Configuration::AudioMode::RTP);
        debug("using RTP audio streaming");
    } else {
        config->setAudioMode(Configuration::AudioMode::Local);
        debug("using local audio playback");
    }

    // RTP fragmentation setting
    auto rtpFragment = program.get<bool>("--rtp-fragment");
    config->setRtpFragmentPackets(rtpFragment);
    debug("RTP packet fragmentation: {}", rtpFragment ? "enabled" : "disabled");

    // Set the GPIO usage
    auto useGPIO = program.get<bool>("-g");
    debug("read use GPIO {} from command line", useGPIO);
    if (useGPIO) {
        config->setUseGPIO(useGPIO);
        debug("set our use GPIO to {}", useGPIO);
    }

    auto mongoURI = program.get<std::string>("-d");
    debug("read mongo URI {} from command line", mongoURI);
    if (!mongoURI.empty()) {
        config->setMongoURI(mongoURI);
        debug("set our mongo URI to {}", mongoURI);
    }

    if (config->getAudioMode() == Configuration::AudioMode::Local) {
        debug("Local audio mode selected, setting local audio playback device");
        auto soundDevice = program.get<int>("-s");
        debug("read sound device {} from command line", soundDevice);
        if (soundDevice >= 0) {
            config->setSoundDevice(soundDevice);
            debug("set our sound device to {}", soundDevice);
        }
    }

    auto soundChannels = program.get<int>("-c");
    debug("read sound channels {} from command line", soundChannels);
    if (soundChannels > 0) {
        config->setSoundChannels(soundChannels);
        debug("set our sound channels to {}", soundChannels);
    }

    auto soundFrequency = program.get<int>("-f");
    debug("read sound frequency {} from command line", soundFrequency);
    if (soundFrequency > 0) {
        config->setSoundFrequency(soundFrequency);
        debug("set our sound frequency to {}", soundFrequency);
    }

    auto soundsLocation = program.get<std::string>("-l");
    debug("read sounds location {} from command line", soundsLocation);
    if (!soundsLocation.empty()) {
        config->setSoundFileLocation(soundsLocation);
        debug("set our sound file location to {}", soundsLocation);
    }

    auto voiceApiKey = program.get<std::string>("-v");
    debug("read voice API key {} from command line", voiceApiKey);
    if (!voiceApiKey.empty()) {
        config->setVoiceApiKey(voiceApiKey);
        debug("set our voice API key to {}", voiceApiKey);
    }

    auto honeycombApiKey = program.get<std::string>("-h");
    debug("read honeycomb API key {} from command line", honeycombApiKey);
    if (!honeycombApiKey.empty()) {
        config->setHoneycombApiKey(honeycombApiKey);
        debug("set our honeycomb API key to {}", honeycombApiKey);
    }

    auto networkDeviceName = program.get<std::string>("-n");
    debug("read network device name {} from command line", networkDeviceName);

    // Try to convert the name to an index number
    try {
        if (uint8_t networkDevice = getInterfaceIndex(networkDeviceName); networkDevice > 0) {
            config->setNetworkDevice(networkDevice);
            debug("set our network device to {}", networkDevice);
        }
    } catch (const std::system_error &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        std::exit(1);
    }

    return config;
}

void CommandLine::listSoundDevices() {

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        const std::string errorMessage = fmt::format("Couldn't initialize SDL: {}\n", SDL_GetError());
        fprintf(stderr, "%s", errorMessage.c_str());
        return;
    }

    const int numDevices = SDL_GetNumAudioDevices(0);
    printf("Number of audio devices found: %d\n", numDevices);

    for (int i = 0; i < numDevices; ++i) {
        if (const char *deviceName = SDL_GetAudioDeviceName(i, 0)) {
            printf(" Device: %d, Name: %s\n", i, deviceName);
        }
    }
}

/**
 * Converts from the friendly name of an interface (like eth0) to the index like the
 * E131Server needs.
 *
 * @param interfaceName the friendly name of an interface like "eth0" or "lo0"
 * @return the index of the interface, or throws if the interface can't be found
 */
uint8_t CommandLine::getInterfaceIndex(const std::string &interfaceName) {
    const uint8_t index = if_nametoindex(interfaceName.c_str());
    if (index == 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to find interface: " + interfaceName);
    }
    return index;
}

void CommandLine::listNetworkDevices() {
    ifaddrs *ifaddr;
    char addrBuff[INET6_ADDRSTRLEN];

    if (getifaddrs(&ifaddr) == -1) {
        critical("Unable to get network devices: {}", strerror(errno));
        return;
    }

    // Map to store device name, index, and IP addresses
    std::map<std::string, std::pair<int, std::vector<std::string>>> interfaces;

    for (const ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        const void *tmpAddrPtr = nullptr;
        bool isIPv4 = false;

        // Check if it is IP4 or IP6 and set tmpAddrPtr accordingly
        if (ifa->ifa_addr->sa_family == AF_INET) { // IPv4
            tmpAddrPtr = &reinterpret_cast<sockaddr_in *>(ifa->ifa_addr)->sin_addr;
            isIPv4 = true;
        } else if (ifa->ifa_addr->sa_family == AF_INET6) { // IPv6
            tmpAddrPtr = &reinterpret_cast<sockaddr_in6 *>(ifa->ifa_addr)->sin6_addr;
        }

        if (tmpAddrPtr) {
            inet_ntop(ifa->ifa_addr->sa_family, tmpAddrPtr, addrBuff, sizeof(addrBuff));
            // Prioritize IPv4 by inserting at the beginning of the vector
            if (isIPv4) {
                interfaces[ifa->ifa_name].second.insert(interfaces[ifa->ifa_name].second.begin(), addrBuff);
            } else {
                interfaces[ifa->ifa_name].second.push_back(addrBuff);
            }
            interfaces[ifa->ifa_name].first = if_nametoindex(ifa->ifa_name);
        }
    }

    freeifaddrs(ifaddr);

    std::cout << "List of network devices:" << std::endl;
    for (const auto &[fst, snd] : interfaces) {
        std::cout << " Name: " << fst;
        std::cout << ", IPs: ";
        for (const auto &ip : snd.second) {
            std::cout << ip << " ";
        }
        std::cout << std::endl;
    }
}

std::string CommandLine::getVersion() {
    return fmt::format("{}.{}.{}", CREATURE_SERVER_VERSION_MAJOR, CREATURE_SERVER_VERSION_MINOR,
                       CREATURE_SERVER_VERSION_PATCH);
}
} // namespace creatures