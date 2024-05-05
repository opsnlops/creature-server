
#pragma once


#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures {

    struct WebsocketMessage {
        std::string command;
        std::string payload;
    };



#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * Data transfer object for a WebsocketMessage
 */
class WebsocketMessageDto : public oatpp::DTO {

    DTO_INIT(WebsocketMessageDto, DTO /* extends */)

    DTO_FIELD_INFO(command) {
        info->description = "The command verb for the message";
        info->required = true;
    }
    DTO_FIELD(String, command);

    DTO_FIELD_INFO(payload) {
        info->description = "A JSON encoded object that contains the payload for the message";
        info->required = true;
    }
    DTO_FIELD(String, payload);


};

#include OATPP_CODEGEN_END(DTO)


    oatpp::Object<WebsocketMessageDto> convertToDto(const WebsocketMessage &websocketMessage);
    WebsocketMessage convertFromDto(const std::shared_ptr<WebsocketMessageDto> &websocketMessageDto);


}