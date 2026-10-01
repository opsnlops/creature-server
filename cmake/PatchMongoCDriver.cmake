# Patch mongo-c-driver's accept() probe so it configures under CMake 4.4+.
#
# libmongoc's mongoc_get_accept_args() passes
#   CMAKE_FLAGS "-Werror -DCMAKE_CXX_LINK_EXECUTABLE='echo not linking now...'"
# to try_compile. That's one argument, which CMake reads as -W<category> with
# the category "error -DCMAKE_CXX_LINK_EXECUTABLE=…". Older CMake ignored the
# unknown category, so the flags were already a no-op; CMake 4.4 rejects it and
# configure fails. Dropping the argument keeps the old behavior everywhere.
# Upstream still has this line as of 1.30.6 / 2.1.2.
#
# Run as PATCH_COMMAND from the source dir. Idempotent, since FetchContent can
# re-run the patch step on an already-patched tree.

set(TARGET_FILE "src/libmongoc/CMakeLists.txt")
set(BAD_FLAGS " CMAKE_FLAGS\n   \"-Werror -DCMAKE_CXX_LINK_EXECUTABLE='echo not linking now...'\"")

file(READ "${TARGET_FILE}" CONTENTS)
string(FIND "${CONTENTS}" "${BAD_FLAGS}" FOUND_AT)
if(FOUND_AT EQUAL -1)
    message(STATUS "mongo-c-driver accept() probe already patched (or changed upstream)")
    return()
endif()

string(REPLACE "${BAD_FLAGS}" "" CONTENTS "${CONTENTS}")
file(WRITE "${TARGET_FILE}" "${CONTENTS}")
message(STATUS "Patched mongo-c-driver accept() probe for CMake 4.4+")
