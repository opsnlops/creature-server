cmake_minimum_required(VERSION 3.18)


project(creature-server
        VERSION "1.0.0"
        DESCRIPTION "Server for April's Creatures"
        HOMEPAGE_URL https://github.com/opsnlops/creature-server
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

set(ABSL_PROPAGATE_CXX_STD on)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

find_package(PkgConfig REQUIRED)

set(CMAKE_POLICY_DEFAULT_CMP0042 NEW)

# grpc
FetchContent_Declare(
        gRPC
        GIT_REPOSITORY https://github.com/grpc/grpc
        GIT_TAG        v1.55.1
)
set(FETCHCONTENT_QUIET OFF)
FetchContent_MakeAvailable(gRPC)
# Since FetchContent uses add_subdirectory under the hood, we can use
# the grpc targets directly from this build.
set(_PROTOBUF_LIBPROTOBUF libprotobuf)
set(_REFLECTION grpc++_reflection)
set(_PROTOBUF_PROTOC $<TARGET_FILE:protoc>)
set(_GRPC_GRPCPP grpc++)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)


# This project uses threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# fmt
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

# MongoDB
find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)

# SDL
find_package(SDL2 REQUIRED)
find_package(SDL2_mixer REQUIRED)


# Proto file
get_filename_component(server_proto "messaging/server.proto" ABSOLUTE)
get_filename_component(server_proto_path "${server_proto}" PATH)

# Generated sources
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
        PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}
        )


# Our source files
file(GLOB serverFiles
        src/exception/*
        src/server/*
        src/server/animation/*
        src/server/creature/*
        src/server/dmx/*
        src/server/eventloop/*
        src/server/eventloop/events/*
        src/server/logging/*
        src/server/sound/*
        src/util/*
        )

file(GLOB clientFiles
        src/client/*
        src/exception/*
        )


file(GLOB logReaderFiles
        src/client/*.h
        src/exception/*
        src/log-monitor/*
        )

# Set up our includes
include_directories(
        src/
        ${LIBMONGOCXX_INCLUDE_DIR}
        ${LIBBSONCXX_INCLUDE_DIR}
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_MIXER_INCLUDE_DIRS}
)

message(STATUS "Using Protocol Buffers ${protobuf_VERSION}")

add_executable(creature-server
        ${serverFiles}
        )

target_link_libraries(creature-server
        PUBLIC
        server_grpc_proto
        mongo::mongocxx_shared
        mongo::bsoncxx_shared
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        absl::flags
        absl::flags_parse
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
        )


add_executable(client
        ${clientFiles}
        )

target_link_libraries(client
        PUBLIC
        server_grpc_proto
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        mongo::bsoncxx_shared
        absl::flags
        absl::flags_parse
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
        )

add_executable(logreader
        ${logReaderFiles}
        )

target_link_libraries(logreader
        PUBLIC
        server_grpc_proto
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        mongo::bsoncxx_shared
        absl::flags
        absl::flags_parse
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
        )

