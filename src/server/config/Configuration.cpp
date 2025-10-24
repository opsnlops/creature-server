// Configuration.cpp
// Implementation of the Configuration class methods

#include "Configuration.h"

#include <utility>

#include "server/namespace-stuffs.h"
#include "util/environment.h"

namespace creatures {

// GPIO Configuration

bool Configuration::getUseGPIO() const { return this->useGPIO; }

void Configuration::setUseGPIO(const bool _useGPIO) { this->useGPIO = _useGPIO; }

// Database Configuration

std::string Configuration::getMongoURI() const { return this->mongoURI; }

void Configuration::setMongoURI(std::string _mongoURI) { this->mongoURI = std::move(_mongoURI); }

// Audio Configuration

uint8_t Configuration::getSoundDevice() const { return this->soundDevice; }

void Configuration::setSoundDevice(const uint8_t _soundDevice) { this->soundDevice = _soundDevice; }

uint32_t Configuration::getSoundFrequency() const { return this->soundFrequency; }

void Configuration::setSoundFrequency(const uint32_t _soundFrequency) { this->soundFrequency = _soundFrequency; }

uint8_t Configuration::getSoundChannels() const { return this->soundChannels; }

void Configuration::setSoundChannels(const uint8_t _soundChannels) { this->soundChannels = _soundChannels; }

std::string Configuration::getSoundFileLocation() const { return this->soundFileLocation; }

void Configuration::setSoundFileLocation(std::string _soundFileLocation) {
    this->soundFileLocation = std::move(_soundFileLocation);
}

Configuration::AudioMode Configuration::getAudioMode() const { return this->audioMode; }

void Configuration::setAudioMode(const Configuration::AudioMode _mode) { this->audioMode = _mode; }

bool Configuration::getRtpFragmentPackets() const { return this->rtpFragmentPackets; }

void Configuration::setRtpFragmentPackets(const bool _fragmentPackets) { this->rtpFragmentPackets = _fragmentPackets; }

// Network Configuration

uint16_t Configuration::getNetworkDevice() const { return this->networkDevice; }

void Configuration::setNetworkDevice(const uint16_t _networkDevice) { this->networkDevice = _networkDevice; }

// External API Configuration

std::string Configuration::getVoiceApiKey() const { return this->voiceApiKey; }

void Configuration::setVoiceApiKey(std::string _voiceApiKey) { this->voiceApiKey = std::move(_voiceApiKey); }

std::string Configuration::getHoneycombApiKey() const { return this->honeycombApiKey; }

void Configuration::setHoneycombApiKey(std::string _honeycombApiKey) {
    this->honeycombApiKey = std::move(_honeycombApiKey);
}

// External Tools Configuration

std::string Configuration::getRhubarbBinaryPath() const { return this->rhubarbBinaryPath; }

void Configuration::setRhubarbBinaryPath(std::string _rhubarbBinaryPath) {
    this->rhubarbBinaryPath = std::move(_rhubarbBinaryPath);
}

std::string Configuration::getFfmpegBinaryPath() const { return this->ffmpegBinaryPath; }

void Configuration::setFfmpegBinaryPath(std::string _ffmpegBinaryPath) {
    this->ffmpegBinaryPath = std::move(_ffmpegBinaryPath);
}

// Observability Configuration

double Configuration::getEventLoopTraceSampling() const { return this->eventLoopTraceSampling; }

void Configuration::setEventLoopTraceSampling(const double _eventLoopTraceSampling) {
    this->eventLoopTraceSampling = _eventLoopTraceSampling;
}

// Animation Scheduler Configuration

Configuration::AnimationSchedulerType Configuration::getAnimationSchedulerType() const {
    return this->animationSchedulerType;
}

void Configuration::setAnimationSchedulerType(const Configuration::AnimationSchedulerType _schedulerType) {
    this->animationSchedulerType = _schedulerType;
}

uint32_t Configuration::getAnimationDelayMs() const { return this->animationDelayMs; }

void Configuration::setAnimationDelayMs(const uint32_t _delayMs) { this->animationDelayMs = _delayMs; }

} // namespace creatures