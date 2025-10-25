

#include <filesystem>

#include "exception/exception.h"

#include <CreatureVoices.h>
#include <VoiceResult.h>
#include <model/CreatureSpeechRequest.h>
#include <model/CreatureSpeechResponse.h>
#include <model/Subscription.h>
#include <model/Voice.h>

#include "server/database.h"

#include "server/config/Configuration.h"
#include "util/ObservabilityManager.h"

#include "VoiceService.h"
#include "server/voice/AudioConverter.h"
#include "server/voice/SpeechGenerationManager.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/MakeSoundFileRequestDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures {
extern std::shared_ptr<creatures::Configuration> config;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures ::ws {

using oatpp::web::protocol::http::Status;

oatpp::Object<ListDto<oatpp::Object<creatures::voice::VoiceDto>>> VoiceService::getAllVoices() {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
    OATPP_COMPONENT(std::shared_ptr<creatures::voice::CreatureVoices>, voiceService);

    appLogger->debug("Asked to return all of the voices");

    bool error = false;
    oatpp::String errorMessage;
    Status status = Status::CODE_200;

    auto voiceResult = voiceService->listAllAvailableVoices();
    if (!voiceResult.isSuccess()) {
        error = true;
        errorMessage = voiceResult.getError()->getMessage();
        switch (voiceResult.getError()->getCode()) {
        case creatures::voice::VoiceError::InvalidApiKey:
            status = Status::CODE_401;
            break;
        case creatures::voice::VoiceError::InvalidData:
            status = Status::CODE_400;
            break;
        default:
            status = Status::CODE_500;
            break;
        }
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    // Looks good, let's keep going!
    auto voices = voiceResult.getValue().value();
    appLogger->debug("Found {} voices", voices.size());

    auto voiceList = oatpp::Vector<oatpp::Object<creatures::voice::VoiceDto>>::createShared();
    for (auto &voice : voices) {
        voiceList->push_back(convertToDto(voice));
        appLogger->trace("adding voice: {}", voice.name);
    }

    auto list = ListDto<oatpp::Object<creatures::voice::VoiceDto>>::createShared();
    list->count = voiceList->size();
    list->items = voiceList;

    return list;
}

oatpp::Object<creatures::voice::SubscriptionDto> VoiceService::getSubscriptionStatus() {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
    OATPP_COMPONENT(std::shared_ptr<creatures::voice::CreatureVoices>, creatureVoices);

    appLogger->debug("Asked to return all of the voices");

    bool error = false;
    oatpp::String errorMessage;
    Status status = Status::CODE_200;

    auto voiceResult = creatureVoices->getSubscriptionStatus();
    if (!voiceResult.isSuccess()) {
        error = true;
        errorMessage = voiceResult.getError()->getMessage();
        switch (voiceResult.getError()->getCode()) {
        case creatures::voice::VoiceError::InvalidApiKey:
            status = Status::CODE_401;
            break;
        case creatures::voice::VoiceError::InvalidData:
            status = Status::CODE_400;
            break;
        default:
            status = Status::CODE_500;
            break;
        }
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    // Looks good, let's keep going!
    auto subscription = voiceResult.getValue().value();
    debug("Found subscription: {}", subscription.status);

    auto dto = convertToDto(subscription);
    return dto;
}

oatpp::Object<creatures::voice::CreatureSpeechResponseDto>
VoiceService::generateCreatureSpeech(const oatpp::Object<MakeSoundFileRequestDto> &soundFileRequest) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
    OATPP_COMPONENT(std::shared_ptr<creatures::voice::CreatureVoices>, creatureVoices);

    appLogger->debug("Incoming request for speech generation");

    auto speechSpan = creatures::observability->createOperationSpan("VoiceService.generateCreatureSpeech");
    if (speechSpan) {
        speechSpan->setAttribute("creature.id", std::string(soundFileRequest->creature_id));
        speechSpan->setAttribute("request.text", std::string(soundFileRequest->text));
    }

    creatures::voice::SpeechGenerationRequest helperRequest;
    helperRequest.creatureId = std::string(soundFileRequest->creature_id);
    helperRequest.text = std::string(soundFileRequest->text);
    helperRequest.title = soundFileRequest->title ? std::string(soundFileRequest->title) : "";
    helperRequest.outputDirectory = std::filesystem::path(config->getSoundFileLocation());
    helperRequest.parentSpan = speechSpan;
    helperRequest.voiceClient = creatureVoices;

    auto helperResult = creatures::voice::SpeechGenerationManager::generate(helperRequest);
    if (!helperResult.isSuccess()) {
        auto error = helperResult.getError().value();
        Status status = Status::CODE_500;
        switch (error.getCode()) {
        case creatures::ServerError::InvalidData:
            status = Status::CODE_400;
            break;
        case creatures::ServerError::NotFound:
            status = Status::CODE_404;
            break;
        default:
            status = Status::CODE_500;
            break;
        }
        if (speechSpan) {
            speechSpan->setError(error.getMessage());
        }
        OATPP_ASSERT_HTTP(false, status, error.getMessage().c_str());
    }

    if (speechSpan) {
        speechSpan->setSuccess();
    }

    return convertToDto(helperResult.getValue().value().response);
}

} // namespace creatures::ws
