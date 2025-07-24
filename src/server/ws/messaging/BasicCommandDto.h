
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

/**
 * A super simple DTO for use in processing messages. This only extracts the command.
 */
class BasicCommandDto : public oatpp::DTO {

    DTO_INIT(BasicCommandDto, DTO)
    DTO_FIELD(String, command);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
