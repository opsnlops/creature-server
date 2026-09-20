# Render docs/transport-route-manifest.json as a byte array header.
#
# A raw string literal would be simpler, but the documented manifest is well
# past the 65,536 characters the C++ standard guarantees for one literal, and
# clang warns about it (-Woverlength-strings). An array initializer has no
# such limit. Invoked with -DINPUT=… -DOUTPUT=… at configure time.

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "EmbedRouteManifest.cmake needs INPUT and OUTPUT")
endif()

file(READ "${INPUT}" MANIFEST_HEX HEX)
string(LENGTH "${MANIFEST_HEX}" HEX_LENGTH)
math(EXPR BYTE_COUNT "${HEX_LENGTH} / 2")

# "0x41,0x42,…" with a line break every 24 bytes so the header stays readable.
# (The file is UTF-8 with bytes above 0x7f, so the array is unsigned.)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," MANIFEST_BYTES "${MANIFEST_HEX}")
# CMake's regex has no bounded repetition, so wrap by inserting a newline after
# every 24th comma-terminated byte (24 * 5 = 120 characters).
string(LENGTH "${MANIFEST_BYTES}" BYTES_LENGTH)
set(WRAPPED "")
set(OFFSET 0)
while(OFFSET LESS BYTES_LENGTH)
    string(SUBSTRING "${MANIFEST_BYTES}" ${OFFSET} 120 LINE)
    string(APPEND WRAPPED "${LINE}\n    ")
    math(EXPR OFFSET "${OFFSET} + 120")
endwhile()
set(MANIFEST_BYTES "${WRAPPED}")

file(WRITE "${OUTPUT}"
"#pragma once

// Generated from docs/transport-route-manifest.json by cmake/EmbedRouteManifest.cmake.
// Do not edit; regenerate by reconfiguring.

#include <cstddef>
#include <string_view>

namespace creatures::transport::generated {

inline constexpr std::size_t ROUTE_MANIFEST_SIZE = ${BYTE_COUNT};

inline constexpr unsigned char ROUTE_MANIFEST_BYTES[ROUTE_MANIFEST_SIZE] = {
    ${MANIFEST_BYTES}
};

// Viewed as chars for the JSON parser; the bytes are UTF-8.
inline const std::string_view ROUTE_MANIFEST_JSON{reinterpret_cast<const char *>(ROUTE_MANIFEST_BYTES),
                                                  ROUTE_MANIFEST_SIZE};

} // namespace creatures::transport::generated
")
