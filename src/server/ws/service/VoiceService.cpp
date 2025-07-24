

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

#include "VoiceService.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/MakeSoundFileRequestDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures {
extern std::shared_ptr<creatures::Configuration> config;
extern std::shared_ptr<Database> db;
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

    bool error = false;
    oatpp::String errorMessage;
    Status status = Status::CODE_200;

    // Go look up the creature we're using in the database
    auto creatureJsonResult = db->getCreatureJson(soundFileRequest->creature_id);
    if (!creatureJsonResult.isSuccess()) {
        error = true;
        errorMessage = creatureJsonResult.getError()->getMessage();
        switch (creatureJsonResult.getError()->getCode()) {
        case creatures::ServerError::NotFound:
            status = Status::CODE_404;
            break;
        case creatures::ServerError::InvalidData:
            status = Status::CODE_400;
            break;
        default:
            status = Status::CODE_500;
            break;
        }
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    // Fetch the creature's voice config from the JSON
    auto creatureJson = creatureJsonResult.getValue().value();

    auto voiceConfig = creatureJson["voice"];
    if (voiceConfig.is_null()) {
        error = true;
        errorMessage = "No voice configuration found for creature";
        status = Status::CODE_400;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    auto speechRequest = creatures::voice::CreatureSpeechRequest();
    try {
        speechRequest.voice_id = voiceConfig["voice_id"].get<std::string>();
        speechRequest.model_id = voiceConfig["model_id"].get<std::string>();
        speechRequest.stability = voiceConfig["stability"].get<float>();
        speechRequest.similarity_boost = voiceConfig["similarity_boost"].get<float>();

        speechRequest.creature_name = creatureJson["name"].get<std::string>();

        speechRequest.title = soundFileRequest->title;
        speechRequest.text = soundFileRequest->text;
    } catch (const std::exception &e) {
        error = true;
        errorMessage = "Failed to parse voice configuration";
        status = Status::CODE_400;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    appLogger->debug("Request is using voice_id: {}", speechRequest.voice_id);

    auto speechResponse =
        creatureVoices->generateCreatureSpeech(std::filesystem::path(config->getSoundFileLocation()), speechRequest);
    if (!speechResponse.isSuccess()) {
        error = true;
        errorMessage = std::string(speechResponse.getError()->getMessage());
        appLogger->error("Failed to generate speech: {}", std::string(errorMessage));
        switch (speechResponse.getError()->getCode()) {
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

    appLogger->debug("Generated speech successfully");
    return convertToDto(speechResponse.getValue().value());
}

} // namespace creatures::ws