


#include "Configuration.h"

#include <utility>

#include "server/namespace-stuffs.h"
#include "util/environment.h"

namespace creatures {


    bool Configuration::getUseGPIO() const {
        return this->useGPIO;
    }
    void Configuration::setUseGPIO(bool _useGPIO) {
        this->useGPIO = _useGPIO;
    }

    std::string Configuration::getMongoURI() const {
        return this->mongoURI;
    }
    void Configuration::setMongoURI(std::string _mongoURI) {
        this->mongoURI = std::move(_mongoURI);
    }

    uint8_t Configuration::getSoundChannels() const {
        return this->soundChannels;
    }
    void Configuration::setSoundChannels(uint8_t _soundChannels) {
        this->soundChannels = _soundChannels;
    }

    uint32_t Configuration::getSoundFrequency() const {
        return this->soundFrequency;
    }
    void Configuration::setSoundFrequency(uint32_t _soundFrequency) {
        this->soundFrequency = _soundFrequency;
    }

    uint8_t Configuration::getSoundDevice() const {
        return this->soundDevice;
    }
    void Configuration::setSoundDevice(uint8_t _soundDevice) {
        this->soundDevice = _soundDevice;
    }

    std::string Configuration::getSoundFileLocation() const {
        return this->soundFileLocation;
    }
    void Configuration::setSoundFileLocation(std::string _soundFileLocation) {
        this->soundFileLocation = std::move(_soundFileLocation);
    }

    uint16_t Configuration::getNetworkDevice() const {
        return this->networkDevice;
    }
    void Configuration::setNetworkDevice(uint16_t _networkDevice) {
        this->networkDevice = _networkDevice;
    }

    std::string Configuration::getVoiceApiKey() const {
        return this->voiceApiKey;
    }
    void Configuration::setVoiceApiKey(std::string _voiceApiKey) {
        this->voiceApiKey = std::move(_voiceApiKey);
    }
}