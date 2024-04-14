cmake_minimum_required(VERSION 3.25)

project(creature-server
        VERSION "1.6.0"
        DESCRIPTION "Server for April's Creatures"
        HOMEPAGE_URL https://github.com/opsnlops/creature-server
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

# Overshadowed declarations keep biting me in the butt 😅
add_compile_options(-Wshadow)
add_compile_options(-Wall)
#add_compile_options(-Wextra)
#add_compile_options(-Wpedantic)

set(PACKAGE_AUTHOR "April White")

# I hate that this has to be done, but I'm in a bad spot with gRPC right now
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set(_CREATURES_GRPC_VERSION 1.60.0)
else()
        set(_CREATURES_GRPC_VERSION 1.60.0)
endif()
message(STATUS "Using gRPC version ${_CREATURES_GRPC_VERSION}")

find_package(PkgConfig REQUIRED)

set(CMAKE_POLICY_DEFAULT_CMP0042 NEW)

# Make our version available to the project
configure_file(src/server/Version.h.in Version.h)

# Log loudly
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# This project uses threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Configure gRPC to use bundled libraries
set(gRPC_ABSL_PROVIDER "module" CACHE STRING "Use bundled ABSL library")
set(gRPC_CARES_PROVIDER "module" CACHE STRING "Use bundled CARES library")
set(gRPC_PROTOBUF_PROVIDER "module" CACHE STRING "Use bundled protobuf library")
set(gRPC_RE2_PROVIDER "module" CACHE STRING "Use bundled RE2 library")
set(gRPC_SSL_PROVIDER "module" CACHE STRING "Use bundled SSL library")
set(gRPC_ZLIB_PROVIDER "module" CACHE STRING "Use bundled ZLIB library")
set(gRPC_INSTALL ON CACHE BOOL "Install gRPC")
set(gRPC_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "Propagate ABSL CXX_STD to subprojects")

# grpc
message(STATUS "Building gRPC ${_CREATURES_GRPC_VERSION}")
FetchContent_Declare(
        gRPC
        GIT_REPOSITORY https://github.com/grpc/grpc
        GIT_TAG        v${_CREATURES_GRPC_VERSION}
)
FetchContent_MakeAvailable(gRPC)
# Since FetchContent uses add_subdirectory under the hood, we can use
# the grpc targets directly from this build.
set(_PROTOBUF_LIBPROTOBUF libprotobuf)
set(_REFLECTION grpc++_reflection)
set(_PROTOBUF_PROTOC $<TARGET_FILE:protoc>)
set(_GRPC_GRPCPP grpc++)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)


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

# json
FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# Google Test
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)


# ...and our own library
add_subdirectory(lib/e131_service)


#
# uWebSockets doesn't use CMake, so we have to do it ourselves. This is
# a bit of a hack, but it works.
#

# Build the stuff that uWebSockets needs
set(UWEBSOCKETS_DIR "${CMAKE_SOURCE_DIR}/lib/uWebSockets")

# Custom command to make the uWebSockets
add_custom_command(
        OUTPUT ${UWEBSOCKETS_DIR}/uSockets/uSockets.a
        COMMAND WITH_BORINGSSL=1 make all
        WORKING_DIRECTORY ${UWEBSOCKETS_DIR}
        COMMENT "Building uWebSockets"
)

# Custom target that depends on the output
add_custom_target(uWebSockets DEPENDS ${UWEBSOCKETS_DIR}/uSockets/uSockets.a)

# Copy the headers to a proper include directory
file(COPY ${UWEBSOCKETS_DIR}/src/
        DESTINATION ${UWEBSOCKETS_DIR}/include/uWebSockets/)


# MongoDB (built by us on Linux, but the macOS/brew version is fine)
find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)


# SDL
find_package(SDL2 REQUIRED)
find_package(SDL2_mixer REQUIRED)

# Why are UUIDs so hard in 2024
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        find_library(UUID uuid REQUIRED)
else()
        pkg_search_module(UUID REQUIRED uuid)
endif()


# Leave a message
message(STATUS "Using SDL2 includes ${SDL2_INCLUDE_DIRS}")

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
        src/server/config/*
        src/server/creature/*
        src/server/eventloop/*
        src/server/eventloop/events/*
        src/server/gpio/*
        src/server/logging/*
        src/server/metrics/*
        src/server/playlist/*
        src/server/sound/*
        src/server/watchdog/*
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
        ${UUID_INCLUDE_DIRS}
        ${UWEBSOCKETS_DIR}/include
        ${UWEBSOCKETS_DIR}/uSockets/src
        ${argparse_SOURCE_DIR}/include
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_MIXER_INCLUDE_DIRS}
        PRIVATE ${CMAKE_BINARY_DIR}
)

#message(STATUS "Using Protocol Buffers ${protobuf_VERSION}")

add_executable(creature-server
        ${serverFiles}
        src/server/watchdog/Watchdog.cpp
        src/server/watchdog/Watchdog.h
        src/server/GrpcServerManager.cpp
        src/server/GrpcServerManager.h
        src/model/Creature.h
        src/model/LogLevel.h
        src/model/LogItem.h
        src/model/FrameData.h
        src/model/AnimationMetadata.h
        src/model/Animation.h
        src/server/ws/WebSocketServer.cpp
        src/server/ws/HttpResponse.h
)
add_dependencies(creature-server uWebSockets)

target_link_libraries(creature-server
        PRIVATE server_grpc_proto
        PRIVATE ${UUID_LIBRARIES}
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        nlohmann_json::nlohmann_json
        PRIVATE mongo::mongocxx_shared
        PRIVATE mongo::bsoncxx_shared
        argparse
        e131_service
        ${UWEBSOCKETS_DIR}/uSockets/uSockets.a
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
)


add_executable(client
        ${clientFiles}
        src/model/Creature.h
        src/model/LogLevel.h
        src/model/LogItem.h
        src/model/FrameData.h
        src/model/AnimationMetadata.h
        src/model/Animation.h
        src/server/ws/HttpResponse.h
)

target_link_libraries(client PRIVATE
        PRIVATE server_grpc_proto
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        nlohmann_json::nlohmann_json
        PRIVATE mongo::mongocxx_shared
        PRIVATE mongo::bsoncxx_shared
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
)

add_executable(logreader
        ${logReaderFiles}
        src/model/LogLevel.h
)

target_link_libraries(logreader PRIVATE
        PRIVATE server_grpc_proto
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        nlohmann_json::nlohmann_json
        PRIVATE mongo::mongocxx_shared
        PRIVATE mongo::bsoncxx_shared
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
)

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



#
# Set up testing
#

enable_testing()

add_executable(creature-server-test
        tests/model/Creature_test.cpp
        tests/model/LogItem_test.cpp
        tests/model/Animation_test.cpp
        tests/model/FrameData_test.cpp
        tests/model/AnimationMetadata_test.cpp
        tests/server/ws/HttpResponse_test.cpp
)

target_link_libraries(creature-server-test
        PRIVATE server_grpc_proto
        PRIVATE ${UUID_LIBRARIES}
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        nlohmann_json::nlohmann_json
        PRIVATE mongo::mongocxx_shared
        PRIVATE mongo::bsoncxx_shared
        argparse
        e131_service
        gtest_main
        gmock_main
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF}
)

# This is to include Google Test and Google Mock headers
target_include_directories(creature-server-test PRIVATE
        ${googletest_SOURCE_DIR}/include
        ${googlemock_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/
)

# Tell CMake where our tests are
set_property(TARGET creature-server-test PROPERTY FOLDER "tests")

# Register the test with CTest
include(GoogleTest)
gtest_discover_tests(creature-server-test)



# where to find our CMake modules
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Package)
