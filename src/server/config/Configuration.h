#pragma once

#include <optional>
#include <string>

#include "server/config.h"

namespace creatures {

/**
 * @class Configuration
 * @brief Manages all configuration settings for the creature server
 *
 * This class centralizes all configuration options including hardware settings,
 * network parameters, database connections, and audio setup. It provides a consistent
 * interface for accessing configuration values throughout the application.
 */
class Configuration {

  public:
    Configuration() = default;
    ~Configuration() = default;

    /**
     * @enum AudioMode
     * @brief Denotes whether audio is played locally or streamed via RTP
     */
    enum class AudioMode {
        Local, ///< Play audio on the local sound device
        RTP    ///< Stream audio via RTP multicast
    };

    /** CommandLine class is allowed to modify configuration settings */
    friend class CommandLine;

    // Getters for configuration properties

    /** @return True if GPIO pins should be used (typically only on Raspberry Pi) */
    bool getUseGPIO() const;

    /** @return MongoDB connection URI string */
    std::string getMongoURI() const;

    /** @return Stable exact native output device name, if configured. */
    std::optional<std::string> getSoundDeviceName() const;
    float getDialogGainDb() const;
    float getBgmGainDb() const;
    float getLimiterCeilingDb() const;
    std::optional<uint8_t> getOutputVolumePercent() const;
    std::string getAlsaMixerCard() const;
    std::string getAlsaMixerElement() const;

    /** @return Audio sampling frequency in Hz */
    uint32_t getSoundFrequency() const;

    /** @return Number of audio channels */
    uint8_t getSoundChannels() const;

    /** @return Directory path where sound files are stored */
    std::string getSoundFileLocation() const;

    /** @return Current audio mode (local playback or RTP streaming) */
    AudioMode getAudioMode() const;

    /** @return True if running in travel mode (server + controllers on one host) */
    bool getTravelMode() const;

    /** @return True if RTP packets should be fragmented for standard MTU networks */
    bool getRtpFragmentPackets() const;

    /** @return Number of fixed workers used for cooperative RTP audio loads */
    uint32_t getRtpAudioLoadWorkers() const;

    /** @return Maximum number of cooperative RTP audio loads waiting for a worker */
    uint32_t getRtpAudioLoadQueueCapacity() const;

    /** @return Network interface device ID for E1.31 communication */
    uint16_t getNetworkDevice() const;

    /** @return API key for voice synthesis service */
    std::string getVoiceApiKey() const;

    /** @return API key for Honeycomb observability service */
    std::string getHoneycombApiKey() const;

    /** @return Path to the Rhubarb Lip Sync binary */
    std::string getRhubarbBinaryPath() const;

    /** @return Sampling rate for event loop tracing (0.0 to 1.0) */
    double getEventLoopTraceSampling() const;

    /** @return Animation delay in milliseconds for audio sync compensation */
    uint32_t getAnimationDelayMs() const;

    /** @return TTL (in hours) for ad-hoc animations */
    uint32_t getAdHocAnimationTtlHours() const;

    /** @return Number of frames to wait before declaring streaming stopped */
    uint32_t getStreamingTimeoutFrames() const;

    /** @return Path to the whisper.cpp GGML model file */
    std::string getWhisperModelPath() const;

    /** @return Path to the CMU Pronouncing Dictionary file */
    std::string getCmuDictPath() const;

    /** @return Lip sync engine to use ("whisper" or "rhubarb") */
    std::string getLipSyncEngine() const;

  protected:
    // Setters used by CommandLine to configure values from command line arguments

    /** @param _useGPIO Whether to use GPIO pins */
    void setUseGPIO(bool _useGPIO);

    /** @param _mongoURI MongoDB connection URI */
    void setMongoURI(std::string _mongoURI);

    void setSoundDeviceName(std::string _soundDeviceName);
    void setDialogGainDb(float _dialogGainDb);
    void setBgmGainDb(float _bgmGainDb);
    void setLimiterCeilingDb(float _limiterCeilingDb);
    void setOutputVolumePercent(uint8_t _outputVolumePercent);
    void setAlsaMixerCard(std::string _alsaMixerCard);
    void setAlsaMixerElement(std::string _alsaMixerElement);

    /** @param _soundFrequency Audio sampling frequency in Hz */
    void setSoundFrequency(uint32_t _soundFrequency);

    /** @param _soundChannels Number of audio channels */
    void setSoundChannels(uint8_t _soundChannels);

    /** @param _soundFileLocation Directory path for sound files */
    void setSoundFileLocation(std::string _soundFileLocation);

    /** @param _networkDevice Network interface device ID */
    void setNetworkDevice(uint16_t _networkDevice);

    /** @param _voiceApiKey API key for voice synthesis service */
    void setVoiceApiKey(std::string _voiceApiKey);

    /** @param _honeycombApiKey API key for Honeycomb observability service */
    void setHoneycombApiKey(std::string _honeycombApiKey);

    /** @param _rhubarbBinaryPath Path to the Rhubarb Lip Sync binary */
    void setRhubarbBinaryPath(std::string _rhubarbBinaryPath);

    /** @param _eventLoopTraceSampling Sampling rate for event loop tracing (0.0 to 1.0) */
    void setEventLoopTraceSampling(double _eventLoopTraceSampling);

    /** @param _mode Audio mode to use (local playback or RTP streaming) */
    void setAudioMode(AudioMode _mode);

    /** @param _travelMode Whether to run in travel mode */
    void setTravelMode(bool _travelMode);

    /** @param _fragmentPackets Whether to enable RTP packet fragmentation */
    void setRtpFragmentPackets(bool _fragmentPackets);

    /** @param _workers Fixed cooperative RTP audio loader worker count */
    void setRtpAudioLoadWorkers(uint32_t _workers);

    /** @param _capacity Maximum queued cooperative RTP audio loads */
    void setRtpAudioLoadQueueCapacity(uint32_t _capacity);

    /** @param _delayMs Animation delay in milliseconds for audio sync compensation */
    void setAnimationDelayMs(uint32_t _delayMs);

    /** @param _ttlHours TTL (hours) for ad-hoc animations */
    void setAdHocAnimationTtlHours(uint32_t _ttlHours);

    /** @param _timeoutFrames Number of frames to wait before declaring streaming stopped */
    void setStreamingTimeoutFrames(uint32_t _timeoutFrames);

    /** @param _whisperModelPath Path to the whisper GGML model file */
    void setWhisperModelPath(std::string _whisperModelPath);

    /** @param _cmuDictPath Path to the CMU Pronouncing Dictionary file */
    void setCmuDictPath(std::string _cmuDictPath);

    /** @param _lipSyncEngine Lip sync engine ("whisper" or "rhubarb") */
    void setLipSyncEngine(std::string _lipSyncEngine);

  private:
    // Hardware configuration

    /** Whether to use GPIO pins (only applicable on Raspberry Pi) */
    bool useGPIO = false;

    // Database configuration

    /** MongoDB connection URI, defaults to value in config.h */
    std::string mongoURI = DEFAULT_DB_URI;

    // Audio configuration

    /** Exact ALSA/CoreAudio device name. Empty selects the platform default. */
    std::optional<std::string> soundDeviceName;
    float dialogGainDb = DEFAULT_DIALOG_GAIN_DB;
    float bgmGainDb = DEFAULT_BGM_GAIN_DB;
    float limiterCeilingDb = DEFAULT_LIMITER_CEILING_DB;
    std::optional<uint8_t> outputVolumePercent;
    std::string alsaMixerCard = DEFAULT_ALSA_MIXER_CARD;
    std::string alsaMixerElement = DEFAULT_ALSA_MIXER_ELEMENT;

    /** Audio sampling frequency in Hz */
    uint32_t soundFrequency = DEFAULT_SOUND_FREQUENCY;

    /** Number of audio channels */
    uint8_t soundChannels = DEFAULT_SOUND_CHANNELS;

    /** Directory path where sound files are stored */
    std::string soundFileLocation = DEFAULT_SOUND_FILE_LOCATION;

    /** Audio mode for playback */
    /** Workshop/default mode is RTP; local output is an explicit or travel-mode opt-in. */
    AudioMode audioMode = AudioMode::RTP;

    /** Travel mode: server and controllers share one Pi; local output is stereo */
    bool travelMode = DEFAULT_TRAVEL_MODE;

    /** Whether to fragment RTP packets for standard MTU networks (WiFi, etc.) */
    bool rtpFragmentPackets = DEFAULT_RTP_FRAGMENT_PACKETS;

    /** Fixed workers for cooperative RTP WAV reads/cache loads */
    uint32_t rtpAudioLoadWorkers = DEFAULT_RTP_AUDIO_LOAD_WORKERS;

    /** Waiting cooperative RTP loads retained in memory before explicit rejection */
    uint32_t rtpAudioLoadQueueCapacity = DEFAULT_RTP_AUDIO_LOAD_QUEUE_CAPACITY;

    // Network configuration

    /** Network interface device ID for E1.31 communication */
    uint16_t networkDevice = 0;

    // External API configuration

    /** API key for ElevenLabs voice synthesis service */
    std::string voiceApiKey = DEFAULT_VOICE_API_KEY;

    /** API key for Honeycomb observability service */
    std::string honeycombApiKey = DEFAULT_HONEYCOMB_API_KEY;

    // External tools configuration

    /** Path to the Rhubarb Lip Sync binary */
    std::string rhubarbBinaryPath = DEFAULT_RHUBARB_BINARY_PATH;

    // Observability configuration

    /** Sampling rate for event loop tracing (0.0 to 1.0) */
    double eventLoopTraceSampling = DEFAULT_EVENT_LOOP_TRACE_SAMPLING;

    // Animation scheduler configuration

    /** Animation delay in milliseconds for audio sync compensation */
    uint32_t animationDelayMs = 0; ///< Default to no delay

    /** TTL (hours) for ad-hoc animations (default 12h) */
    uint32_t adHocAnimationTtlHours = DEFAULT_ADHOC_ANIMATION_TTL_HOURS;

    /** Timeout (frames) after the last stream frame before marking streaming stopped */
    uint32_t streamingTimeoutFrames = DEFAULT_STREAMING_TIMEOUT_FRAMES;

    // Lip sync configuration

    /** Path to the whisper.cpp GGML model file (empty = whisper not available) */
    std::string whisperModelPath = DEFAULT_WHISPER_MODEL_PATH;

    /** Path to the CMU Pronouncing Dictionary file */
    std::string cmuDictPath = DEFAULT_CMU_DICT_PATH;

    /** Lip sync engine: "whisper" or "rhubarb" */
    std::string lipSyncEngine = DEFAULT_LIP_SYNC_ENGINE;
};

} // namespace creatures
