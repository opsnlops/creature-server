cmake_minimum_required(VERSION 3.25)

project(creature-server
        VERSION "1.2.2"
        DESCRIPTION "Server for April's Creatures"
        HOMEPAGE_URL https://github.com/opsnlops/creature-server
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

include(FetchContent)
find_package(PkgConfig REQUIRED)
set(FETCHCONTENT_QUIET OFF)

set(CMAKE_POLICY_DEFAULT_CMP0042 NEW)

# Include the stuff we build earlier like how a TV chef will
# pull an already made turkey out from under the table 🍗
set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};${CMAKE_SOURCE_DIR}/external/install")
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(utf8_range CONFIG REQUIRED)

# Make our version available to the project
configure_file(src/server/Version.h.in Version.h)

#
# Dependencies
#
#  These are ones that are pre-compiled and don't need to be
#  compiled, only found and used.
#

# Threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# MongoDB (built by us on Linux, but the macOS/brew version is fine)
find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)

# SDL
find_package(SDL2 REQUIRED)
find_package(SDL2_mixer REQUIRED)
message(STATUS "Using SDL2 includes ${SDL2_INCLUDE_DIRS}")

# Why are UUIDs so hard in 2024
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        find_library(UUID uuid REQUIRED)
else()
        pkg_search_module(UUID REQUIRED uuid)
endif()


#
# Header Only Libraries
#

# fmt
message(STATUS "fmt")
FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt
        GIT_TAG 9.1.0
)
FetchContent_MakeAvailable(fmt)
set(FMT_HEADER_ONLY ON)
set(FMT_LOCALE ON)

# spdlog
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog
        GIT_TAG v1.11.0
)
FetchContent_MakeAvailable(spdlog)

# argparse
FetchContent_Declare(
        argparse
        GIT_REPOSITORY https://github.com/p-ranav/argparse.git
)
FetchContent_MakeAvailable(argparse)



# ...and our own library
add_subdirectory(lib/e131_service)


# Proto file
get_filename_component(server_proto "messaging/server.proto" ABSOLUTE)
get_filename_component(server_proto_path "${server_proto}" PATH)


# Find Protoc and gRPC plugin executables
find_program(_PROTOBUF_PROTOC NAMES protoc PATHS "${CMAKE_SOURCE_DIR}/external/install/bin" NO_DEFAULT_PATH)
find_program(_GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin PATHS "${CMAKE_SOURCE_DIR}/external/install/bin" NO_DEFAULT_PATH)

# Error checking
if(NOT _PROTOBUF_PROTOC)
        message(FATAL_ERROR "protoc not found")
endif()
if(NOT _GRPC_CPP_PLUGIN_EXECUTABLE)
        message(FATAL_ERROR "grpc_cpp_plugin not found")
endif()
message(STATUS "Using protoc ${_PROTOBUF_PROTOC}")
message(STATUS "Using grpc_cpp_plugin ${_GRPC_CPP_PLUGIN_EXECUTABLE}")
link_directories(${CMAKE_SOURCE_DIR}/external/install/lib)


set(server_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/server.pb.cc")
set(server_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/server.pb.h")
set(server_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/server.grpc.pb.cc")
set(server_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/server.grpc.pb.h")
add_custom_command(
        OUTPUT "${server_proto_srcs}" "${server_proto_hdrs}" "${server_grpc_srcs}" "${server_grpc_hdrs}"
        COMMAND ${_PROTOBUF_PROTOC}
        ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${server_proto_path}"
        -I "${grpc_SOURCE_DIR}/third_party/protobuf/src"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${server_proto}"
        DEPENDS "${server_proto}")

add_library(server_grpc_proto
        ${server_grpc_srcs}
        ${server_grpc_hdrs}
        ${server_proto_srcs}
        ${server_proto_hdrs}
)
target_link_libraries(server_grpc_proto
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
)
target_include_directories(server_grpc_proto
        PUBLIC ${CMAKE_CURRENT_BINARY_DIR}
)


# Macros to make stuff easier
set(_PROTOBUF_LIBPROTOBUF protobuf)
set(_REFLECTION grpc++_reflection)
set(_GRPC_GRPCPP grpc++)


# Our source files
file(GLOB serverFiles
        src/exception/*
        src/server/*
        src/server/animation/*
        src/server/config/*
        src/server/creature/*
        src/server/dmx/*
        src/server/eventloop/*
        src/server/eventloop/events/*
        src/server/gpio/*
        src/server/logging/*
        src/server/metrics/*
        src/server/playlist/*
        src/server/sound/*
        src/util/*
)

file(GLOB clientFiles
        src/client/*
        src/exception/*
        src/util/*
)


file(GLOB logReaderFiles
        src/client/*.h
        src/exception/*
        src/log-monitor/*
)

# Set up our includes
include_directories(
        src/
        ${CMAKE_SOURCE_DIR}/external/install/include
        ${UUID_INCLUDE_DIRS}
        ${argparse_SOURCE_DIR}/include
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_MIXER_INCLUDE_DIRS}
        PRIVATE ${CMAKE_BINARY_DIR}
)

message(STATUS "Using Protocol Buffers ${protobuf_VERSION}")

add_executable(creature-server
        ${serverFiles}
)
target_link_directories(
        creature-server
        PRIVATE ${CMAKE_SOURCE_DIR}/external/install/lib
)
target_link_libraries(creature-server
        PUBLIC server_grpc_proto
        grpc++
        grpc
        protobuf
        absl::base
        absl::algorithm
        absl::cleanup
        absl::debugging
        absl::flags
        absl::hash
        absl::log
        absl::memory
        absl::meta
        absl::numeric
        absl::status
        absl::strings
        absl::synchronization
        absl::time
        absl::utility
        PRIVATE ${UUID_LIBRARIES}
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        PRIVATE mongo::mongocxx_shared
        PRIVATE mongo::bsoncxx_shared
        argparse
        e131_service
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
)




#add_executable(client
#        ${clientFiles}
#)
#
#target_link_libraries(client
#        PRIVATE server_grpc_proto
#        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
#        fmt::fmt
#        PRIVATE mongo::mongocxx_shared
#        PRIVATE mongo::bsoncxx_shared
#        ${_REFLECTION}
#        ${_GRPC_GRPCPP}
#        ${_PROTOBUF_LIBPROTOBUF}
#)

#add_executable(logreader
#        ${logReaderFiles}
#)
#
#target_link_libraries(logreader
#        PRIVATE server_grpc_proto
#        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
#        fmt::fmt
#        PRIVATE mongo::mongocxx_shared
#       PRIVATE mongo::bsoncxx_shared
#        ${_REFLECTION}
#        ${_GRPC_GRPCPP}
#        ${_PROTOBUF_LIBPROTOBUF}
#)

install(TARGETS creature-server
        COMPONENT creature-server
        RUNTIME DESTINATION "/bin"
        LIBRARY DESTINATION "/lib"
        DESTINATION "/bin"
)

# Include the MongoDB C++ driver
install(DIRECTORY /usr/local/lib
        DESTINATION /usr
        COMPONENT creature-server
        PATTERN "python" EXCLUDE
)


# where to find our CMake modules
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Package)
