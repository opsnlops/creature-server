#pragma once

#include <string>

#include "server/config.h"


namespace creatures {

    class Configuration {

    public:

        Configuration() = default;
        ~Configuration() = default;

        friend class CommandLine;

        bool getUseGPIO() const;
        std::string getMongoURI() const;

        uint8_t getSoundDevice() const;
        uint32_t getSoundFrequency() const;
        uint8_t getSoundChannels() const;
        std::string getSoundFileLocation() const;
        uint16_t getNetworkDevice() const;


    protected:
        void setUseGPIO(bool _useGPIO);
        void setMongoURI(std::string _mongoURI);

        void setSoundDevice(uint8_t _soundDevice);
        void setSoundFrequency(uint32_t _soundFrequency);
        void setSoundChannels(uint8_t _soundChannels);
        void setSoundFileLocation(std::string _soundFileLocation);
        void setNetworkDevice(uint16_t _networkDevice);

    private:

        // Should we use the GPIO pins?
        bool useGPIO = false;

        // Where to go to find the database
        std::string mongoURI = DEFAULT_DB_URI;


        // Sound stuff
        uint8_t soundDevice = DEFAULT_SOUND_DEVICE_NUMBER;
        uint32_t soundFrequency = DEFAULT_SOUND_FREQUENCY;
        uint8_t soundChannels = DEFAULT_SOUND_CHANNELS;
        std::string soundFileLocation = DEFAULT_SOUND_FILE_LOCATION;

        // Network stuff
        uint16_t networkDevice = DEFAULT_NETWORK_DEVICE_NUMBER;
    };


}