#pragma once

#include <string>
#include <string_view>

namespace creatures::transport {

/** A transport-neutral OpenAPI document generated from the frozen route catalog. */
const std::string &openApiDocument();

/** A dependency-free, locally packaged browser for openApiDocument(). */
std::string_view apiBrowserHtml();

} // namespace creatures::transport
