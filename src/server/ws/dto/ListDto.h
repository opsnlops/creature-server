
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

template <class T> class ListDto : public oatpp::DTO {

    DTO_INIT(ListDto, DTO)

    DTO_FIELD(UInt32, count);
    DTO_FIELD(Vector<T>, items);
};

#include OATPP_CODEGEN_END(DTO)
