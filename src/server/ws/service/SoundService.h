
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/Sound.h"

#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures :: ws {

    class SoundService {

    private:
        typedef oatpp::web::protocol::http::Status Status;

    public:

        /**
         * Play a sound file for testing
         *
         * @param soundFile
         * @return
         */
        oatpp::Object<creatures::ws::StatusDto> playSound(const oatpp::String& soundFile);

        /**
         * Get all of the sound files
         */
        oatpp::Object<ListDto<oatpp::Object<creatures::SoundDto>>> getAllSounds();

    };


} // creatures :: ws
