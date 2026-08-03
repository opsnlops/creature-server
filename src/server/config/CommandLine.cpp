#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ifaddrs.h>
#include <iostream>
#include <net/if.h>
#include <netinet/in.h>
#include <string>
#include <system_error>
#include <utility>

#include <argparse/argparse.hpp>
#include <fmt/format.h>

#include "CommandLine.h"
#include "Configuration.h"

#include "Version.h"

#include "server/audio/AudioOutput.h"
#include "server/audio/NativeAudioConfig.h"
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

    program.add_argument("--audio-device-name")
        .help("exact ALSA/CoreAudio output name from --list-sound-devices")
        .default_value(environmentToString(SOUND_DEVICE_NAME_ENV, DEFAULT_SOUND_DEVICE_NAME))
        .nargs(1);

    program.add_argument("--dialog-gain-db")
        .help("software gain for dialog channels in dB (-90 to +12)")
        .default_value(environmentToDouble(DIALOG_GAIN_DB_ENV, DEFAULT_DIALOG_GAIN_DB))
        .scan<'g', double>()
        .nargs(1);

    program.add_argument("--bgm-gain-db")
        .help("software gain for the BGM channel in dB (-90 to +12)")
        .default_value(environmentToDouble(BGM_GAIN_DB_ENV, DEFAULT_BGM_GAIN_DB))
        .scan<'g', double>()
        .nargs(1);

    program.add_argument("--limiter-ceiling-db")
        .help("post-mix limiter ceiling in dB (-90 to 0)")
        .default_value(environmentToDouble(LIMITER_CEILING_DB_ENV, DEFAULT_LIMITER_CEILING_DB))
        .scan<'g', double>()
        .nargs(1);

    program.add_argument("--output-volume-percent")
        .help("optional ALSA hardware playback volume (0 to 100)")
        .default_value(environmentToInt(OUTPUT_VOLUME_PERCENT_ENV, -1))
        .scan<'i', int>()
        .nargs(1);

    program.add_argument("--alsa-mixer-card")
        .help("ALSA mixer card used for --output-volume-percent")
        .default_value(environmentToString(ALSA_MIXER_CARD_ENV, DEFAULT_ALSA_MIXER_CARD))
        .nargs(1);

    program.add_argument("--alsa-mixer-element")
        .help("ALSA playback element used for --output-volume-percent")
        .default_value(environmentToString(ALSA_MIXER_ELEMENT_ENV, DEFAULT_ALSA_MIXER_ELEMENT))
        .nargs(1);

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

    program.add_argument("-r", "--rhubarb-binary-path")
        .help("path to the Rhubarb Lip Sync binary")
        .default_value(environmentToString(RHUBARB_BINARY_PATH_ENV, DEFAULT_RHUBARB_BINARY_PATH))
        .nargs(1);

    program.add_argument("--whisper-model-path")
        .help("path to the whisper.cpp GGML model file (e.g., ggml-base.en.bin)")
        .default_value(environmentToString(WHISPER_MODEL_PATH_ENV, DEFAULT_WHISPER_MODEL_PATH))
        .nargs(1);

    program.add_argument("--cmu-dict-path")
        .help("path to the CMU Pronouncing Dictionary file")
        .default_value(environmentToString(CMU_DICT_PATH_ENV, DEFAULT_CMU_DICT_PATH))
        .nargs(1);

    program.add_argument("--lip-sync-engine")
        .help("lip sync engine to use: 'whisper' (fast, library) or 'rhubarb' (legacy, subprocess)")
        .default_value(environmentToString(LIP_SYNC_ENGINE_ENV, DEFAULT_LIP_SYNC_ENGINE))
        .nargs(1);

    program.add_argument("-h", "--honeycomb-api-key")
        .help("Honeycomb API key")
        .default_value(environmentToString(HONEYCOMB_API_KEY_ENV, DEFAULT_HONEYCOMB_API_KEY))
        .nargs(1);

    program.add_argument("--event-loop-trace-sampling")
        .help("sampling rate for event loop tracing (0.0 to 1.0)")
        .default_value(environmentToDouble(EVENT_LOOP_TRACE_SAMPLING_ENV, DEFAULT_EVENT_LOOP_TRACE_SAMPLING))
        .scan<'g', double>()
        .nargs(1);

    program.add_argument("--streaming-timeout-frames")
        .help("number of 1ms frames to wait before marking streaming as stopped")
        .default_value(environmentToInt(STREAMING_TIMEOUT_FRAMES_ENV, DEFAULT_STREAMING_TIMEOUT_FRAMES))
        .scan<'i', int>()
        .nargs(1);

    program.add_argument("--rtp-fragment")
        .help("enable RTP packet fragmentation for standard MTU networks (WiFi, etc.)")
        .default_value(environmentToInt(RTP_FRAGMENT_PACKETS_ENV, DEFAULT_RTP_FRAGMENT_PACKETS) == 1)
        .implicit_value(true);

    program.add_argument("--rtp-audio-load-workers")
        .help("fixed worker count for cooperative RTP WAV/cache loads")
        .default_value(environmentToInt(RTP_AUDIO_LOAD_WORKERS_ENV, DEFAULT_RTP_AUDIO_LOAD_WORKERS))
        .scan<'i', int>()
        .nargs(1);

    program.add_argument("--rtp-audio-load-queue-capacity")
        .help("maximum cooperative RTP audio loads waiting for a worker")
        .default_value(environmentToInt(RTP_AUDIO_LOAD_QUEUE_CAPACITY_ENV, DEFAULT_RTP_AUDIO_LOAD_QUEUE_CAPACITY))
        .scan<'i', int>()
        .nargs(1);

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
        .help("use native local audio playback instead of the default RTP transport")
        .default_value(false)
        .implicit_value(true);

    audioMode.add_argument("--rtp-audio")
        .help("use RTP audio streaming (default)")
        .default_value(false)
        .implicit_value(true);

    /*
     * Travel mode collapses the whole rig onto one host: the server and the controllers
     * all run on the same Pi. It forces local stereo audio (dialog lanes 1/2,
     * with BGM lane 17 on both) and keeps sACN multicast from leaving the host. The HTTP and
     * websocket server still binds 0.0.0.0 so the console can attach over the network.
     */
    program.add_argument("--travel-mode")
        .help("single-Pi mode: host-local sACN and stereo local audio (dialog lanes 1/2 plus BGM)")
        .default_value(environmentToInt(TRAVEL_MODE_ENV, DEFAULT_TRAVEL_MODE) == 1)
        .implicit_value(true);

    program.add_argument("--animation-delay-ms")
        .help("delay in milliseconds before starting animation playback (for audio sync compensation)")
        .default_value(0)
        .scan<'i', int>()
        .nargs(1);

    program.add_argument("--adhoc-animation-ttl-hours")
        .help("number of hours to retain ad-hoc animations (Mongo TTL + temp files)")
        .default_value(environmentToInt(ADHOC_ANIMATION_TTL_HOURS_ENV, DEFAULT_ADHOC_ANIMATION_TTL_HOURS))
        .scan<'i', int>()
        .nargs(1);

    program.add_description("Creature Server for April's Creature Workshop\n\n"
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

    // Do the sound and network listing(s) first to avoid weird debug messages
    if (program.get<bool>("--list-sound-devices")) {
        listSoundDevices();
        std::exit(0);
    }

    if (program.get<bool>("--list-network-devices")) {
        listNetworkDevices();
        std::exit(0);
    }

    debug("Parsing the command line options");

    // The always-on workshop host defaults to RTP. Travel mode is the explicit
    // single-Pi deployment and always owns its ALSA output locally.
    const bool travelMode = program.get<bool>("--travel-mode");
    const bool explicitLocalAudio = program.get<bool>("--local-audio");
    const bool explicitRtpAudio = program.get<bool>("--rtp-audio");
    std::string configuredAudioMode = environmentToString(AUDIO_MODE_ENV, DEFAULT_AUDIO_MODE);
    std::transform(configuredAudioMode.begin(), configuredAudioMode.end(), configuredAudioMode.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (configuredAudioMode != "rtp" && configuredAudioMode != "local") {
        critical("{} must be 'rtp' or 'local'", AUDIO_MODE_ENV);
        std::exit(1);
    }
    if (travelMode) {
        if (explicitRtpAudio) {
            critical("--travel-mode and --rtp-audio cannot be used together; travel mode plays a stereo mix on "
                     "the local sound device");
            std::exit(1);
        }
        config->setTravelMode(true);
        info("travel mode enabled: host-local sACN, stereo local audio with BGM on both channels");
    }

    if (travelMode || explicitLocalAudio || (!explicitRtpAudio && configuredAudioMode == "local")) {
        config->setAudioMode(Configuration::AudioMode::Local);
        debug("using local audio playback");
    } else {
        config->setAudioMode(Configuration::AudioMode::RTP);
        debug("using RTP audio streaming");
    }

    // RTP fragmentation setting
    auto rtpFragment = program.get<bool>("--rtp-fragment");
    config->setRtpFragmentPackets(rtpFragment);
    debug("RTP packet fragmentation: {}", rtpFragment ? "enabled" : "disabled");

    auto rtpAudioLoadWorkers = program.get<int>("--rtp-audio-load-workers");
    const bool rtpAudioLoadWorkersOverridden =
        program.is_used("--rtp-audio-load-workers") || std::getenv(RTP_AUDIO_LOAD_WORKERS_ENV) != nullptr;
    if (travelMode && !rtpAudioLoadWorkersOverridden) {
        // Travel mode currently forces local audio, but keep its effective
        // default safe for the Pi if that restriction changes later.
        rtpAudioLoadWorkers = 1;
    }
    if (rtpAudioLoadWorkers < 1 || rtpAudioLoadWorkers > 32) {
        critical("--rtp-audio-load-workers must be between 1 and 32");
        std::exit(1);
    }
    config->setRtpAudioLoadWorkers(static_cast<uint32_t>(rtpAudioLoadWorkers));

    auto rtpAudioLoadQueueCapacity = program.get<int>("--rtp-audio-load-queue-capacity");
    if (rtpAudioLoadQueueCapacity < 1 || rtpAudioLoadQueueCapacity > 1024) {
        critical("--rtp-audio-load-queue-capacity must be between 1 and 1024");
        std::exit(1);
    }
    config->setRtpAudioLoadQueueCapacity(static_cast<uint32_t>(rtpAudioLoadQueueCapacity));
    debug("RTP audio loader configured with {} workers and {} queued jobs", rtpAudioLoadWorkers,
          rtpAudioLoadQueueCapacity);

    // Animation delay for audio sync compensation
    auto animationDelayMs = program.get<int>("--animation-delay-ms");
    if (animationDelayMs < 0) {
        critical("--animation-delay-ms must be non-negative");
        std::exit(1);
    }
    config->setAnimationDelayMs(static_cast<uint32_t>(animationDelayMs));
    if (animationDelayMs > 0) {
        debug("animation playback will be delayed by {}ms for audio sync", animationDelayMs);
    }

    auto streamingTimeoutFrames = program.get<int>("--streaming-timeout-frames");
    if (streamingTimeoutFrames <= 0) {
        critical("--streaming-timeout-frames must be greater than zero");
        std::exit(1);
    }
    config->setStreamingTimeoutFrames(static_cast<uint32_t>(streamingTimeoutFrames));
    debug("streaming timeout set to {} frames (~{}ms)", streamingTimeoutFrames, streamingTimeoutFrames);

    auto adHocTtlHours = program.get<int>("--adhoc-animation-ttl-hours");
    if (adHocTtlHours <= 0) {
        critical("--adhoc-animation-ttl-hours must be greater than zero");
        std::exit(1);
    }
    config->setAdHocAnimationTtlHours(static_cast<uint32_t>(adHocTtlHours));
    debug("ad-hoc animation TTL set to {} hours", adHocTtlHours);

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

    const auto soundDeviceName = program.get<std::string>("--audio-device-name");
    if (soundDeviceName.size() > 512) {
        critical("--audio-device-name must not exceed 512 characters");
        std::exit(1);
    }
    if (!soundDeviceName.empty()) {
        config->setSoundDeviceName(soundDeviceName);
        debug("set native audio device name to '{}'", soundDeviceName);
    }

    const auto dialogGainDb = program.get<double>("--dialog-gain-db");
    const auto bgmGainDb = program.get<double>("--bgm-gain-db");
    const auto limiterCeilingDb = program.get<double>("--limiter-ceiling-db");
    if (!std::isfinite(dialogGainDb) || !std::isfinite(bgmGainDb) || dialogGainDb < audio::MIN_AUDIO_GAIN_DB ||
        dialogGainDb > audio::MAX_AUDIO_GAIN_DB || bgmGainDb < audio::MIN_AUDIO_GAIN_DB ||
        bgmGainDb > audio::MAX_AUDIO_GAIN_DB) {
        critical("--dialog-gain-db and --bgm-gain-db must be finite and between -90 and +12");
        std::exit(1);
    }
    if (!std::isfinite(limiterCeilingDb) || limiterCeilingDb < audio::MIN_AUDIO_GAIN_DB || limiterCeilingDb > 0.0) {
        critical("--limiter-ceiling-db must be finite and between -90 and 0");
        std::exit(1);
    }
    config->setDialogGainDb(static_cast<float>(dialogGainDb));
    config->setBgmGainDb(static_cast<float>(bgmGainDb));
    config->setLimiterCeilingDb(static_cast<float>(limiterCeilingDb));

    const auto outputVolumePercent = program.get<int>("--output-volume-percent");
    if (outputVolumePercent < -1 || outputVolumePercent > 100) {
        critical("--output-volume-percent must be between 0 and 100");
        std::exit(1);
    }
    if (outputVolumePercent >= 0) {
        config->setOutputVolumePercent(static_cast<uint8_t>(outputVolumePercent));
    }
    config->setAlsaMixerCard(program.get<std::string>("--alsa-mixer-card"));
    config->setAlsaMixerElement(program.get<std::string>("--alsa-mixer-element"));

    const auto soundChannels = program.get<int>("-c");
    debug("read sound channels {} from command line", soundChannels);
    if (travelMode) {
        if (program.is_used("-c") && soundChannels != 2) {
            warn("ignoring --sound-channels {} in travel mode; travel output is stereo", soundChannels);
        }
        config->setSoundChannels(2);
        debug("travel mode: sound channels forced to 2 (stereo)");
    } else if (soundChannels >= 1 && soundChannels <= 64) {
        config->setSoundChannels(soundChannels);
        debug("set our sound channels to {}", soundChannels);
    } else {
        critical("--sound-channels must be between 1 and 64");
        std::exit(1);
    }

    const auto soundFrequency = program.get<int>("-f");
    debug("read sound frequency {} from command line", soundFrequency);
    if (soundFrequency >= 8000 && soundFrequency <= 384000) {
        config->setSoundFrequency(soundFrequency);
        debug("set our sound frequency to {}", soundFrequency);
    } else {
        critical("--sound-frequency must be between 8000 and 384000");
        std::exit(1);
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

    auto rhubarbBinaryPath = program.get<std::string>("-r");
    debug("read rhubarb binary path {} from command line", rhubarbBinaryPath);
    if (!rhubarbBinaryPath.empty()) {
        config->setRhubarbBinaryPath(rhubarbBinaryPath);
        debug("set our rhubarb binary path to {}", rhubarbBinaryPath);
    }

    auto honeycombApiKey = program.get<std::string>("-h");
    debug("read honeycomb API key {} from command line", honeycombApiKey);
    if (!honeycombApiKey.empty()) {
        config->setHoneycombApiKey(honeycombApiKey);
        debug("set our honeycomb API key to {}", honeycombApiKey);
    }

    auto eventLoopTraceSampling = program.get<double>("--event-loop-trace-sampling");
    debug("read event loop trace sampling rate {} from command line", eventLoopTraceSampling);
    config->setEventLoopTraceSampling(eventLoopTraceSampling);
    debug("set event loop trace sampling rate to {}", eventLoopTraceSampling);

    auto whisperModelPath = program.get<std::string>("--whisper-model-path");
    if (!whisperModelPath.empty()) {
        config->setWhisperModelPath(whisperModelPath);
        debug("set whisper model path to {}", whisperModelPath);
    }

    auto cmuDictPath = program.get<std::string>("--cmu-dict-path");
    if (!cmuDictPath.empty()) {
        config->setCmuDictPath(cmuDictPath);
        debug("set CMU dictionary path to {}", cmuDictPath);
    }

    auto lipSyncEngine = program.get<std::string>("--lip-sync-engine");
    if (lipSyncEngine != "whisper" && lipSyncEngine != "rhubarb") {
        critical("--lip-sync-engine must be 'whisper' or 'rhubarb', got '{}'", lipSyncEngine);
        std::exit(1);
    }
    config->setLipSyncEngine(lipSyncEngine);
    debug("set lip sync engine to {}", lipSyncEngine);

    auto networkDeviceName = program.get<std::string>("-n");
    debug("read network device name {} from command line", networkDeviceName);

    // Try to convert the name to an index number
    try {
        if (uint8_t networkDevice = getInterfaceIndex(networkDeviceName); networkDevice > 0) {
            config->setNetworkDevice(networkDevice);
            debug("set our network device to {}", networkDevice);
        }
    } catch (const std::system_error &e) {
        critical(e.what());
        std::exit(1);
    }

    return config;
}

void CommandLine::listSoundDevices() { audio::listAudioOutputDevices(std::cout); }

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
