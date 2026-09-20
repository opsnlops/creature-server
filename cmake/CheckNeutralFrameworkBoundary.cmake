if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

# oat++ was removed in 3.50.0. Nothing under src/ or tests/ may mention it
# again; the uWebSockets transport is the only HTTP/WebSocket implementation.
set(NEUTRAL_ROOTS
    "${SOURCE_ROOT}/src"
    "${SOURCE_ROOT}/tests"
)

set(NEUTRAL_FILES)

foreach(ROOT IN LISTS NEUTRAL_ROOTS)
    file(GLOB_RECURSE ROOT_FILES LIST_DIRECTORIES false
        "${ROOT}/*.c"
        "${ROOT}/*.cc"
        "${ROOT}/*.cpp"
        "${ROOT}/*.h"
        "${ROOT}/*.hh"
        "${ROOT}/*.hpp"
        "${ROOT}/*.py"
    )
    list(APPEND NEUTRAL_FILES ${ROOT_FILES})
endforeach()

set(VIOLATIONS)
foreach(FILE_PATH IN LISTS NEUTRAL_FILES)
    file(READ "${FILE_PATH}" CONTENTS)
    string(TOLOWER "${CONTENTS}" LOWER_CONTENTS)
    string(FIND "${LOWER_CONTENTS}" "oatpp" MATCH_INDEX)
    if(NOT MATCH_INDEX EQUAL -1)
        file(RELATIVE_PATH RELATIVE_FILE "${SOURCE_ROOT}" "${FILE_PATH}")
        list(APPEND VIOLATIONS "${RELATIVE_FILE}")
    endif()
endforeach()

if(VIOLATIONS)
    list(JOIN VIOLATIONS "\n  " FORMATTED_VIOLATIONS)
    message(FATAL_ERROR
        "oat++ references found (the oat++ transport was removed in 3.50.0):\n  ${FORMATTED_VIOLATIONS}"
    )
endif()

message(STATUS "Framework-neutral boundary check passed")
