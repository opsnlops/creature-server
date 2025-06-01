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
    
    /** @return Network interface device ID for E1.31 communication */
    uint16_t getNetworkDevice() const;
    
    /** @return API key for voice synthesis service */
    std::string getVoiceApiKey() const;

    /** @return API key for Honeycomb observability service */
    std::string getHoneycombApiKey() const;

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

    // Network configuration
    
    /** Network interface device ID for E1.31 communication */
    uint16_t networkDevice = 0;

    // External API configuration
    
    /** API key for ElevenLabs voice synthesis service */
    std::string voiceApiKey = DEFAULT_VOICE_API_KEY;

    /** API key for Honeycomb observability service */
    std::string honeycombApiKey = DEFAULT_HONEYCOMB_API_KEY;
};

} // namespace creatures