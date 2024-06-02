
#include <string>

#include <oatpp/core/Types.hpp>

#include "Subscription.h"

namespace creatures::voice {

    Subscription convertFromDto(const std::shared_ptr<SubscriptionDto> &voiceDto) {
        Subscription subscription;
        subscription.tier = voiceDto->tier;
        subscription.status = voiceDto->status;
        subscription.character_count = voiceDto->character_count;
        subscription.character_limit = voiceDto->character_limit;

        return subscription;
    }

    // Convert this into its DTO
    oatpp::Object<SubscriptionDto> convertToDto(const Subscription &voice) {
        auto subscriptionDto = SubscriptionDto::createShared();
        subscriptionDto->tier = voice.tier;
        subscriptionDto->status = voice.status;
        subscriptionDto->character_count = voice.character_count;
        subscriptionDto->character_limit = voice.character_limit;

        return subscriptionDto;
    }

}