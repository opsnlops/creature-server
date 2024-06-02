
#pragma once

#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>


namespace creatures :: voice {

    struct Subscription {
        std::string tier;
        std::string status;
        uint32_t character_count;
        uint32_t character_limit;
    };

#include OATPP_CODEGEN_BEGIN(DTO)

    class SubscriptionDto : public oatpp::DTO {

        DTO_INIT(SubscriptionDto, DTO /* extends */)

        DTO_FIELD_INFO(tier) {
            info->description = "Which tier the subscription is at";
        }
        DTO_FIELD(String, tier);

        DTO_FIELD_INFO(status) {
            info->description = "The status of the subscription";
        }
        DTO_FIELD(String, status);

        DTO_FIELD_INFO(character_count) {
            info->description = "The number of characters used in this billing cycle";
        }
        DTO_FIELD(UInt32, character_count);

        DTO_FIELD_INFO(character_limit) {
            info->description = "The number of characters allowed in this billing cycle";
        }
        DTO_FIELD(UInt32, character_limit);

    };

#include OATPP_CODEGEN_END(DTO)


    oatpp::Object<SubscriptionDto> convertToDto(const Subscription &subscription);
    Subscription convertFromDto(const std::shared_ptr<SubscriptionDto> &subscriptionDto);


}

