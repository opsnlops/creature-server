

#include <iostream>
#include <filesystem>


#include "exception/exception.h"


#include <model/Voice.h>
#include <CreatureVoices.h>
#include <VoiceResult.h>

#include "server/config/Configuration.h"


#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

#include "util/helpers.h"


#include "VoiceService.h"


namespace creatures {
    extern std::shared_ptr<creatures::Configuration> config;
}

namespace creatures :: ws {

    using oatpp::web::protocol::http::Status;


    oatpp::Object<ListDto<oatpp::Object<creatures::voice::VoiceDto>>> VoiceService::getAllVoices() {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
        OATPP_COMPONENT(std::shared_ptr<creatures::voice::CreatureVoices>, voiceService);

        appLogger->debug("Asked to return all of the voices");


        bool error = false;
        oatpp::String errorMessage;
        Status status = Status::CODE_200;


        auto voiceResult = voiceService->listAllAvailableVoices();
        if(!voiceResult.isSuccess()) {
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
        debug("Found {} voices", voices.size());

        auto voiceList = oatpp::Vector<oatpp::Object<creatures::voice::VoiceDto>>::createShared();
        for(auto& voice : voices) {
            voiceList->push_back(convertToDto(voice));
            trace("adding voice: {}", voice.name);
        }

        auto list = ListDto<oatpp::Object<creatures::voice::VoiceDto>>::createShared();
        list->count = voiceList->size();
        list->items = voiceList;

        return list;

    }

}