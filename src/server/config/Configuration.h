#pragma once

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

    /**
     * @enum AnimationSchedulerType
     * @brief Denotes which animation scheduler implementation to use
     */
    enum class AnimationSchedulerType {
        Legacy,     ///< Bulk event scheduling (original implementation)
        Cooperative ///< Cooperative frame-by-frame scheduling (new implementation)
    };

    /** CommandLine class is allowed to modify configuration settings */
    friend class CommandLine;

    // Getters for configuration properties

    /** @return True if GPIO pins should be used (typically only on Raspberry Pi) */
    bool getUseGPIO() const;

    /** @return MongoDB connection URI string */
    std::string getMongoURI() const;

    /** @return The sound device number to use */
    uint8_t getSoundDevice() const;

    /** @return Audio sampling frequency in Hz */
    uint32_t getSoundFrequency() const;

    /** @return Number of audio channels */
    uint8_t getSoundChannels() const;

    /** @return Directory path where sound files are stored */
    std::string getSoundFileLocation() const;

    /** @return Current audio mode (local playback or RTP streaming) */
    AudioMode getAudioMode() const;

    /** @return True if RTP packets should be fragmented for standard MTU networks */
    bool getRtpFragmentPackets() const;

    /** @return Network interface device ID for E1.31 communication */
    uint16_t getNetworkDevice() const;

    /** @return API key for voice synthesis service */
    std::string getVoiceApiKey() const;

    /** @return API key for Honeycomb observability service */
    std::string getHoneycombApiKey() const;

    /** @return Path to the Rhubarb Lip Sync binary */
    std::string getRhubarbBinaryPath() const;

    /** @return Path to the ffmpeg binary */
    std::string getFfmpegBinaryPath() const;

    /** @return Sampling rate for event loop tracing (0.0 to 1.0) */
    double getEventLoopTraceSampling() const;

    /** @return Animation scheduler type to use */
    AnimationSchedulerType getAnimationSchedulerType() const;

  protected:
    // Setters used by CommandLine to configure values from command line arguments

    /** @param _useGPIO Whether to use GPIO pins */
    void setUseGPIO(bool _useGPIO);

    /** @param _mongoURI MongoDB connection URI */
    void setMongoURI(std::string _mongoURI);

    /** @param _soundDevice Sound device number */
    void setSoundDevice(uint8_t _soundDevice);

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

    /** @param _ffmpegBinaryPath Path to the ffmpeg binary */
    void setFfmpegBinaryPath(std::string _ffmpegBinaryPath);

    /** @param _eventLoopTraceSampling Sampling rate for event loop tracing (0.0 to 1.0) */
    void setEventLoopTraceSampling(double _eventLoopTraceSampling);

    /** @param _mode Audio mode to use (local playback or RTP streaming) */
    void setAudioMode(AudioMode _mode);

    /** @param _fragmentPackets Whether to enable RTP packet fragmentation */
    void setRtpFragmentPackets(bool _fragmentPackets);

    /** @param _schedulerType Animation scheduler type to use */
    void setAnimationSchedulerType(AnimationSchedulerType _schedulerType);

  private:
    // Hardware configuration

    /** Whether to use GPIO pins (only applicable on Raspberry Pi) */
    bool useGPIO = false;

    // Database configuration

    /** MongoDB connection URI, defaults to value in config.h */
    std::string mongoURI = DEFAULT_DB_URI;

    // Audio configuration

    /** Sound device number for audio output */
    uint8_t soundDevice = DEFAULT_SOUND_DEVICE_NUMBER;

    /** Audio sampling frequency in Hz */
    uint32_t soundFrequency = DEFAULT_SOUND_FREQUENCY;

    /** Number of audio channels */
    uint8_t soundChannels = DEFAULT_SOUND_CHANNELS;

    /** Directory path where sound files are stored */
    std::string soundFileLocation = DEFAULT_SOUND_FILE_LOCATION;

    /** Audio mode for playback */
    AudioMode audioMode = AudioMode::Local; ///< Default to local playback

    /** Whether to fragment RTP packets for standard MTU networks (WiFi, etc.) */
    bool rtpFragmentPackets = DEFAULT_RTP_FRAGMENT_PACKETS;

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

    /** Path to the ffmpeg binary */
    std::string ffmpegBinaryPath = DEFAULT_FFMPEG_BINARY_PATH;

    // Observability configuration

    /** Sampling rate for event loop tracing (0.0 to 1.0) */
    double eventLoopTraceSampling = DEFAULT_EVENT_LOOP_TRACE_SAMPLING;

    // Animation scheduler configuration

    /** Animation scheduler type */
    AnimationSchedulerType animationSchedulerType = AnimationSchedulerType::Legacy; ///< Default to legacy for safety
};

} // namespace creatures