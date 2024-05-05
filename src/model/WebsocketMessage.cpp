


#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>


#include "WebsocketMessage.h"

namespace creatures {

    WebsocketMessage convertFromDto(const std::shared_ptr<WebsocketMessageDto> &websocketMessageDto) {
        WebsocketMessage message;  // Create an instance of FrameData
        message.command = websocketMessageDto->command;
        message.payload = websocketMessageDto->payload;

        return message;
    }

    // Convert FrameData to FrameDataDTO
    oatpp::Object<WebsocketMessageDto> convertToDto(const WebsocketMessage &websocketMessage) {

        auto messageDTO = WebsocketMessageDto::createShared();
        messageDTO->command = websocketMessage.command;
        messageDTO->payload = websocketMessage.payload;

        return messageDTO;
    }

}