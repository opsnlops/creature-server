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

std::optional<std::string> Configuration::getSoundDeviceName() const { return soundDeviceName; }
void Configuration::setSoundDeviceName(std::string value) { soundDeviceName = std::move(value); }
float Configuration::getDialogGainDb() const { return dialogGainDb; }
void Configuration::setDialogGainDb(float value) { dialogGainDb = value; }
float Configuration::getBgmGainDb() const { return bgmGainDb; }
void Configuration::setBgmGainDb(float value) { bgmGainDb = value; }
float Configuration::getLimiterCeilingDb() const { return limiterCeilingDb; }
void Configuration::setLimiterCeilingDb(float value) { limiterCeilingDb = value; }
std::optional<uint8_t> Configuration::getOutputVolumePercent() const { return outputVolumePercent; }
void Configuration::setOutputVolumePercent(uint8_t value) { outputVolumePercent = value; }
std::string Configuration::getAlsaMixerCard() const { return alsaMixerCard; }
void Configuration::setAlsaMixerCard(std::string value) { alsaMixerCard = std::move(value); }
std::string Configuration::getAlsaMixerElement() const { return alsaMixerElement; }
void Configuration::setAlsaMixerElement(std::string value) { alsaMixerElement = std::move(value); }

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

bool Configuration::getTravelMode() const { return this->travelMode; }

void Configuration::setTravelMode(const bool _travelMode) { this->travelMode = _travelMode; }

bool Configuration::getRtpFragmentPackets() const { return this->rtpFragmentPackets; }

void Configuration::setRtpFragmentPackets(const bool _fragmentPackets) { this->rtpFragmentPackets = _fragmentPackets; }

uint32_t Configuration::getRtpAudioLoadWorkers() const { return this->rtpAudioLoadWorkers; }

void Configuration::setRtpAudioLoadWorkers(const uint32_t _workers) { this->rtpAudioLoadWorkers = _workers; }

uint32_t Configuration::getRtpAudioLoadQueueCapacity() const { return this->rtpAudioLoadQueueCapacity; }

void Configuration::setRtpAudioLoadQueueCapacity(const uint32_t _capacity) {
    this->rtpAudioLoadQueueCapacity = _capacity;
}

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

// Observability Configuration

double Configuration::getEventLoopTraceSampling() const { return this->eventLoopTraceSampling; }

void Configuration::setEventLoopTraceSampling(const double _eventLoopTraceSampling) {
    this->eventLoopTraceSampling = _eventLoopTraceSampling;
}

// Animation Scheduler Configuration

uint32_t Configuration::getAnimationDelayMs() const { return this->animationDelayMs; }

void Configuration::setAnimationDelayMs(const uint32_t _delayMs) { this->animationDelayMs = _delayMs; }

uint32_t Configuration::getAdHocAnimationTtlHours() const { return this->adHocAnimationTtlHours; }

void Configuration::setAdHocAnimationTtlHours(const uint32_t _ttlHours) { this->adHocAnimationTtlHours = _ttlHours; }

uint32_t Configuration::getStreamingTimeoutFrames() const { return this->streamingTimeoutFrames; }

void Configuration::setStreamingTimeoutFrames(const uint32_t _timeoutFrames) {
    this->streamingTimeoutFrames = _timeoutFrames;
}

// Lip Sync Configuration

std::string Configuration::getWhisperModelPath() const { return this->whisperModelPath; }

void Configuration::setWhisperModelPath(std::string _whisperModelPath) {
    this->whisperModelPath = std::move(_whisperModelPath);
}

std::string Configuration::getCmuDictPath() const { return this->cmuDictPath; }

void Configuration::setCmuDictPath(std::string _cmuDictPath) { this->cmuDictPath = std::move(_cmuDictPath); }

std::string Configuration::getLipSyncEngine() const { return this->lipSyncEngine; }

void Configuration::setLipSyncEngine(std::string _lipSyncEngine) { this->lipSyncEngine = std::move(_lipSyncEngine); }

} // namespace creatures
