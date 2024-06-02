#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

// From our CreatureVoiceLib
#include <model/Voice.h>

#include "server/ws/dto/ListDto.h"


namespace creatures :: ws {

    class VoiceService {

    private:
        typedef oatpp::web::protocol::http::Status Status;

    public:

        VoiceService() = default;
        virtual ~VoiceService() = default;

        /**
         * Get all of the voices
         */
        oatpp::Object<ListDto<oatpp::Object<creatures::voice::VoiceDto>>> getAllVoices();

    };



} // creatures :: ws
