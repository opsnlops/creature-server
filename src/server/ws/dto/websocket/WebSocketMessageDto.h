

#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>


namespace creatures :: ws {

#include OATPP_CODEGEN_BEGIN(DTO)

    template<class T>
    class WebSocketMessageDto : public oatpp::DTO {

        DTO_INIT(WebSocketMessageDto, DTO)

        DTO_FIELD(String, command);
        DTO_FIELD(T, payload);

    };

} // creatures :: ws


#include OATPP_CODEGEN_END(DTO)
