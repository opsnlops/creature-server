
#pragma once

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

    /**
     * This is one message that's sent from the client to control a creature in real time. They are
     * sent over the websocket.
     *
     * This is different than `FrameData` because it is not connected to a specific animation, and contains
     * only one frame of data.
     */
    struct StreamFrame {
        creatureId_t creature_id;
        universe_t universe;
        std::string data;  // The frame data will be base64 encoded strings
    };


#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * Data transfer object for FrameData
 */
class StreamFrameDto : public oatpp::DTO {

    DTO_INIT(StreamFrameDto, DTO /* extends */)


    DTO_FIELD_INFO(creature_id) {
        info->description = "The ID of the creature we are streaming to. This might not be validated during a stream.";
        info->required = true;
    }

    DTO_FIELD(String, creature_id);


    DTO_FIELD_INFO(universe) {
        info->description = "The universe to stream to this creature in";
        info->required = true;
    }

    DTO_FIELD(UInt32, universe);

    DTO_FIELD_INFO(data) {
        info->description = "A base64 encoded string that represents the requested joint positions";
        info->required = true;
    }

    DTO_FIELD(String, data);

};

#include OATPP_CODEGEN_END(DTO)


    oatpp::Object<StreamFrameDto> convertToDto(const StreamFrame &streamFrame);

    StreamFrame convertFromDto(const std::shared_ptr<StreamFrameDto> &streamFrameDto);


}