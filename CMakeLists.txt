cmake_minimum_required(VERSION 3.25)

project(creature-server
        VERSION "2.4.27"
        DESCRIPTION "Server for April's Creatures"
        HOMEPAGE_URL https://github.com/opsnlops/creature-server
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

# Store warning flags to apply to our targets only (not dependencies)
set(CREATURE_SERVER_WARNING_FLAGS
    -Wshadow
    -Wall
    -Wextra
    -Wpedantic
)

set(PACKAGE_AUTHOR "April White")

find_package(PkgConfig REQUIRED)

set(CMAKE_POLICY_DEFAULT_CMP0042 NEW)

# Make our version available to the project
configure_file(src/server/Version.h.in Version.h)

# Log loudly
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Configure MongoDB drivers for static linking BEFORE fetching
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
set(ENABLE_STATIC ON CACHE BOOL "Build static mongoc" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "Don't build shared mongoc" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "Don't build mongoc tests" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "Don't build mongoc examples" FORCE)
set(ENABLE_AUTOMATIC_INIT_AND_CLEANUP OFF CACHE BOOL "Disable automatic init" FORCE)
set(ENABLE_UNINSTALL OFF CACHE BOOL "Don't generate uninstaller" FORCE)
set(MONGOCXX_ENABLE_SHARED OFF CACHE BOOL "Don't build shared mongocxx" FORCE)
set(MONGOCXX_ENABLE_STATIC ON CACHE BOOL "Build static mongocxx" FORCE)
set(BSONCXX_POLY_USE_BOOST OFF CACHE BOOL "Don't use boost" FORCE)
set(BSONCXX_POLY_USE_MNMLSTC OFF CACHE BOOL "Don't use mnmlstc" FORCE)

# Fetch MongoDB C driver
FetchContent_Declare(
  mongo-c-driver
  GIT_REPOSITORY https://github.com/mongodb/mongo-c-driver.git
  GIT_TAG        1.30.4
  GIT_SHALLOW    TRUE
)

# Fetch MongoDB C++ driver
FetchContent_Declare(
  mongo-cxx-driver
  GIT_REPOSITORY https://github.com/mongodb/mongo-cxx-driver.git
  GIT_TAG        r3.11.0
  GIT_SHALLOW    TRUE
)

# Build both drivers
FetchContent_MakeAvailable(mongo-c-driver)
FetchContent_MakeAvailable(mongo-cxx-driver)

# Create aliases that match the existing code
if(TARGET mongocxx_static)
  add_library(mongocxx::mongocxx ALIAS mongocxx_static)
endif()

if(TARGET bsoncxx_static)
  add_library(bsoncxx::bsoncxx ALIAS bsoncxx_static)
endif()

# This project uses threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Why are UUIDs so hard in 2025?
if(NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    pkg_search_module(UUID REQUIRED uuid)
endif()

# Force O3 optimization for Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
        set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
    endif()
endif()


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

# uvgRTP
FetchContent_Declare(
  uvgrtp
  GIT_REPOSITORY https://github.com/ultravideo/uvgRTP.git
  GIT_TAG        v3.1.6
)
FetchContent_MakeAvailable(uvgrtp)

# Opus
# Force Opus' own CMake options *before* the project is configured
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_STATIC         ON  CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS       OFF CACHE BOOL "" FORCE)   # cli tools
set(OPUS_BUILD_TESTING        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        opus
        GIT_REPOSITORY  https://github.com/xiph/opus.git
        GIT_TAG         v1.5.2
)
FetchContent_MakeAvailable(opus)
# Optimise Opus even when the rest of the build is Debug
target_compile_options(opus PRIVATE
        $<$<CONFIG:Debug>:-O3>
)


# Google Test
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# Now configure OpenTelemetry with very specific settings
set(BUILD_TESTING OFF CACHE BOOL "Disable testing" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "Disable examples" FORCE)
set(WITH_BENCHMARK OFF CACHE BOOL "Disable benchmarks" FORCE)
set(WITH_FUNC_TESTS OFF CACHE BOOL "Disable functional tests" FORCE)
set(OPENTELEMETRY_INSTALL OFF CACHE BOOL "Disable install" FORCE)
set(WITH_GTEST OFF CACHE BOOL "Disable gtest in OpenTelemetry" FORCE)

# Enable what we need for traces AND metrics
set(WITH_OTLP_HTTP ON CACHE BOOL "Enable OTLP HTTP exporter" FORCE)
set(WITH_OTLP_GRPC OFF CACHE BOOL "Disable OTLP gRPC exporter" FORCE)

# Enable metrics support
set(WITH_METRICS_API ON CACHE BOOL "Enable metrics API" FORCE)
set(WITH_METRICS_SDK ON CACHE BOOL "Enable metrics SDK" FORCE)
set(WITH_OTLP_HTTP_METRIC_EXPORT ON CACHE BOOL "Enable OTLP HTTP metric exporter" FORCE)

# Disable everything else to avoid dependencies
set(WITH_ZIPKIN OFF CACHE BOOL "Disable Zipkin" FORCE)
set(WITH_JAEGER OFF CACHE BOOL "Disable Jaeger" FORCE)
set(WITH_PROMETHEUS OFF CACHE BOOL "Disable Prometheus" FORCE)
set(WITH_METRICS_PREVIEW OFF CACHE BOOL "Disable metrics preview" FORCE)
set(WITH_LOGS_PREVIEW OFF CACHE BOOL "Disable logs preview" FORCE)

# OpenTelemetry
FetchContent_Declare(
        opentelemetry-cpp
        GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
        GIT_TAG v1.21.0
        GIT_SHALLOW TRUE
)

# After FetchContent_Declare but before FetchContent_MakeAvailable
set(OPENTELEMETRY_BUILD_SHARED_LIBS OFF CACHE BOOL "Build OpenTelemetry as static" FORCE)

FetchContent_MakeAvailable(opentelemetry-cpp)

# Use the oatpp that we've already made
find_package(oatpp           REQUIRED PATHS ${CMAKE_SOURCE_DIR}/externals/install/lib/cmake/oatpp-1.3.0)
find_package(oatpp-swagger   REQUIRED PATHS ${CMAKE_SOURCE_DIR}/externals/install/lib/cmake/oatpp-swagger-1.3.0)
find_package(oatpp-websocket REQUIRED PATHS ${CMAKE_SOURCE_DIR}/externals/install/lib/cmake/oatpp-websocket-1.3.0)

set(oatpp_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/externals/install/include/oatpp-1.3.0/")
message(STATUS "oatpp_INCLUDE_DIRS: ${oatpp_INCLUDE_DIRS}")

add_definitions(
        ## define path to swagger-ui static resources folder
        -DOATPP_SWAGGER_RES_PATH="${oatpp-swagger_INCLUDE_DIRS}/../bin/oatpp-swagger/res"
)

# ...and our own libraries
add_subdirectory(lib/e131_service)
add_subdirectory(lib/CreatureVoicesLib)

# Set up a base64 encoder/decoder
set(LIBBASE64_DIR "${CMAKE_SOURCE_DIR}/lib/base64")

# Set up a concurrent queue
set(MOODYCAMEL_DIR "${CMAKE_SOURCE_DIR}/lib/concurrentqueue")

#
# If we're on macOS, use the local copy of the Swagger UI. On Linux, use the one from the
# package that we installed.
#
if(APPLE)
    add_definitions(-DSWAGGER_UI_PATH="${CMAKE_SOURCE_DIR}/externals/build/oatpp-swagger-prefix/src/oatpp-swagger/res/")
else()
    add_definitions(-DSWAGGER_UI_PATH="/usr/share/creature-server/swagger-ui/")
endif()

# SDL
find_package(SDL2 REQUIRED)
find_package(SDL2_mixer REQUIRED)

# OpenSSL for SHA-256 hashing in audio cache
find_package(OpenSSL REQUIRED)

# Leave a message
message(STATUS "Using SDL2 includes ${SDL2_INCLUDE_DIRS}")

# Our source files
file(GLOB serverFiles
        src/util/*
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
        src/server/watchdog/*
        src/model/*
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
        util/
        ${UUID_INCLUDE_DIRS}
        ${argparse_SOURCE_DIR}/include
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_MIXER_INCLUDE_DIRS}
        ${MOODYCAMEL_DIR}
        ${LIBBASE64_DIR}/include
        ${oatpp_INCLUDE_DIRS}
        PRIVATE ${CMAKE_BINARY_DIR}
)

add_executable(creature-server
        ${serverFiles}
        src/server/watchdog/Watchdog.cpp
        src/server/watchdog/Watchdog.h

        src/model/LogLevel.h
        src/model/LogItem.h

        src/model/AnimationMetadata.cpp
        src/model/AnimationMetadata.h
        src/model/Animation.cpp
        src/model/Animation.h
        src/model/CacheInvalidation.h
        src/model/CacheInvalidation.cpp
        src/model/Creature.h
        src/model/Creature.cpp
        src/model/Input.h
        src/model/Input.cpp
        src/model/LogLevel.h
        src/model/LogItem.h
        src/model/LogItem.cpp
        src/model/Notice.h
        src/model/Notice.cpp
        src/model/Playlist.h
        src/model/Playlist.cpp
        src/model/PlaylistItem.h
        src/model/PlaylistItem.cpp
        src/model/PlaylistStatus.h
        src/model/PlaylistStatus.cpp
        src/model/StreamFrame.cpp
        src/model/StreamFrame.h
        src/model/Sound.h
        src/model/Sound.cpp
        src/model/SortBy.h
        src/model/Track.cpp
        src/model/Track.h
        src/model/VirtualStatusLights.cpp
        src/model/VirtualStatusLights.h

        src/util/loggingUtils.cpp
        src/util/loggingUtils.h
        src/util/Result.cpp
        src/util/Result.h
        src/util/websocketUtils.h
        src/util/websocketUtils.cpp

        src/server/ws/AppComponent.h
        src/server/ws/App.h
        src/server/ws/App.cpp

        src/server/ws/dto/ListDto.h
        src/server/ws/dto/PlayAnimationRequestDto.h
        src/server/ws/dto/PlaySoundRequestDTO.h
        src/server/ws/dto/SimpleResponseDto.h
        src/server/ws/dto/StatusDto.h

        src/server/ws/ErrorHandler.cpp
        src/server/ws/ErrorHandler.h

        src/server/ws/controller/CreatureController.h
        src/server/ws/controller/DebugController.h
        src/server/ws/controller/MetricsController.h
        src/server/ws/controller/SoundController.h
        src/server/ws/controller/StaticController.h
        src/server/ws/controller/WebSocketController.h

        src/server/ws/dto/websocket/CacheInvalidationMessage.h
        src/server/ws/dto/websocket/LogMessage.h
        src/server/ws/dto/websocket/MessageTypes.h
        src/server/ws/dto/websocket/MessageTypes.cpp
        src/server/ws/dto/websocket/NoticeMessage.h
        src/server/ws/dto/websocket/ServerCountersMessage.h
        src/server/ws/dto/websocket/StreamFrameMessage.h
        src/server/ws/dto/websocket/VirtualStatusLightsMessage.h
        src/server/ws/dto/websocket/WebSocketMessageDto.h

        src/server/ws/messaging/BasicCommandDto.h
        src/server/ws/messaging/IMessageHandler.h
        src/server/ws/messaging/NoticeMessageHandler.cpp
        src/server/ws/messaging/NoticeMessageHandler.h
        src/server/ws/messaging/MessageProcessor.h
        src/server/ws/messaging/MessageProcessor.cpp
        src/server/ws/messaging/StreamFrameHandler.h
        src/server/ws/messaging/StreamFrameHandler.cpp

        src/server/ws/service/AnimationService.h
        src/server/ws/service/AnimationService.cpp
        src/server/ws/service/CreatureService.h
        src/server/ws/service/CreatureService.cpp
        src/server/ws/service/MetricsService.h
        src/server/ws/service/MetricsService.cpp
        src/server/ws/service/PlaylistService.h
        src/server/ws/service/PlaylistService.cpp
        src/server/ws/service/SoundService.h
        src/server/ws/service/SoundService.cpp
        src/server/ws/service/VoiceService.h
        src/server/ws/service/VoiceService.cpp

        src/server/ws/websocket/ClientCafe.h
        src/server/ws/websocket/ClientConnection.h
        src/server/ws/websocket/ClientCafe.cpp
        src/server/ws/websocket/ClientConnection.cpp
        src/server/ws/messaging/NoticeMessageCommandDTO.h

        src/server/ws/messaging/SensorReportHandler.h
        src/server/ws/messaging/SensorReportHandler.cpp
        src/server/ws/messaging/SensorReportCommandDTO.h

        src/server/sensors/SensorDataCache.h
        src/server/sensors/SensorDataCache.cpp
        src/util/ObservabilityManager.h
        src/util/ObservabilityManager.cpp
        src/util/JsonParser.h
        src/util/JsonParser.cpp
        src/server/rtp/AudioStreamBuffer.cpp
        src/server/rtp/AudioStreamBuffer.h
        src/server/rtp/AudioChunk.h
        src/server/rtp/opus/OpusEncoderWrapper.cpp
        src/server/rtp/opus/OpusEncoderWrapper.h
        src/server/rtp/MultiOpusRtpServer.cpp
        src/server/rtp/MultiOpusRtpServer.h
)

target_compile_definitions(creature-server PRIVATE OATPP_THREAD_DISTRIBUTED)

# Apply strict warning flags to our main executable only
target_compile_options(creature-server PRIVATE ${CREATURE_SERVER_WARNING_FLAGS})

# System libraries needed for MongoDB static linking
if(NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(MONGO_SYSTEM_LIBS resolv)
else()
    set(MONGO_SYSTEM_LIBS resolv)
endif()

target_link_libraries(creature-server
        PRIVATE ${UUID_LIBRARIES}
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        oatpp::oatpp
        oatpp::oatpp-swagger
        oatpp::oatpp-websocket
        nlohmann_json::nlohmann_json
        mongocxx::mongocxx
        bsoncxx::bsoncxx
        # OpenTelemetry libraries for traces
        opentelemetry_trace
        opentelemetry_exporter_otlp_http
        opentelemetry_resources
        opentelemetry_common
        opentelemetry_http_client_curl  # Needed for HTTP exporter
        opentelemetry_metrics
        opentelemetry_exporter_otlp_http_metric
        argparse
        e131_service
        CreatureVoicesLib
        Threads::Threads
        uvgrtp
        opus
        OpenSSL::SSL
        OpenSSL::Crypto
        ${MONGO_SYSTEM_LIBS}
)

install(TARGETS creature-server
        COMPONENT creature-server
        RUNTIME DESTINATION "/bin"
        LIBRARY DESTINATION "/lib"
        DESTINATION "/bin"
)

#
# Set up testing
#

enable_testing()

add_executable(creature-server-test
        tests/model/Creature_test.cpp
        tests/model/LogItem_test.cpp
        tests/model/Animation_test.cpp
        tests/model/AnimationMetadata_test.cpp
        src/model/SortBy.h
        src/model/Track.cpp
)

target_link_libraries(creature-server-test
        PRIVATE ${UUID_LIBRARIES}
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        oatpp::oatpp
        oatpp::oatpp-swagger
        oatpp::oatpp-websocket
        nlohmann_json::nlohmann_json
        mongocxx::mongocxx
        bsoncxx::bsoncxx
        argparse
        e131_service
        CreatureVoicesLib
        gtest_main
        gmock_main
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        ${MONGO_SYSTEM_LIBS}
)

# Apply strict warning flags to our test executable too
target_compile_options(creature-server-test PRIVATE ${CREATURE_SERVER_WARNING_FLAGS})

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
