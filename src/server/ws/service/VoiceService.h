#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

// From our CreatureVoiceLib
#include <model/Subscription.h>
#include <model/Voice.h>

#include "server/ws/dto/MakeSoundFileRequestDto.h"
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


        /**
         * Gets the status of our subscription
         */
        oatpp::Object<creatures::voice::SubscriptionDto> getSubscriptionStatus();


        /**
         * Generate a sound file for a creature based on the text given
         *
         * @param speechRequest the request to generate the speech
         * @return A VoiceResult with information about what happened
         */
        oatpp::Object<creatures::voice::CreatureSpeechResponseDto> generateCreatureSpeech(const oatpp::Object<MakeSoundFileRequestDto> &soundFileRequest);
    };



} // creatures :: ws
