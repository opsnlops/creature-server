cmake_minimum_required(VERSION 3.25)

project(creature-server
        VERSION "2.2.2"
        DESCRIPTION "Server for April's Creatures"
        HOMEPAGE_URL https://github.com/opsnlops/creature-server
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

# Overshadowed declarations keep biting me in the butt 😅
add_compile_options(-Wshadow)
add_compile_options(-Wall)
add_compile_options(-Wextra)
add_compile_options(-Wpedantic)

set(PACKAGE_AUTHOR "April White")


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

# Why are UUIDs so hard in 2024
if(NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    pkg_search_module(UUID REQUIRED uuid)
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

# Google Test
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)


# Use the oatpp that we've already made
find_package(oatpp           REQUIRED PATHS ${CMAKE_SOURCE_DIR}/externals/install/lib/cmake/oatpp-1.3.0)
#find_package(oatpp-swagger   REQUIRED PATHS ${CMAKE_SOURCE_DIR}/externals/install/lib/cmake/oatpp-swagger-1.3.0)
find_package(oatpp-websocket REQUIRED PATHS ${CMAKE_SOURCE_DIR}/externals/install/lib/cmake/oatpp-websocket-1.3.0)

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


# MongoDB (built by us on Linux, but the macOS/brew version is fine)
find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)


# SDL
find_package(SDL2 REQUIRED)
find_package(SDL2_mixer REQUIRED)



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
        src/server/ws/controller/MetricsController.h
        src/server/ws/controller/SoundController.h
        src/server/ws/controller/StaticController.h
        src/server/ws/controller/WebSocketController.h

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
        src/server/ws/dto/websocket/CacheInvalidationMessage.h


)

target_compile_definitions(creature-server PRIVATE OATPP_THREAD_DISTRIBUTED)


target_link_libraries(creature-server
        PRIVATE ${UUID_LIBRARIES}
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        oatpp::oatpp
#        oatpp::oatpp-swagger
        oatpp::oatpp-websocket
        nlohmann_json::nlohmann_json
        PRIVATE mongo::mongocxx_shared
        PRIVATE mongo::bsoncxx_shared
        argparse
        e131_service
        CreatureVoicesLib
)


#add_executable(client
#        ${clientFiles}
#        src/model/Creature.h
#        src/model/LogLevel.h
#        src/model/LogItem.h
#        src/model/FrameData.h
#        src/model/AnimationMetadata.h
#        src/model/Animation.h
#)
#
#target_link_libraries(client PRIVATE
#        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
#        fmt::fmt
#        oatpp::oatpp
#        nlohmann_json::nlohmann_json
#        PRIVATE mongo::mongocxx_shared
#        PRIVATE mongo::bsoncxx_shared
#)

#add_executable(logreader
#        ${logReaderFiles}
#        src/model/LogLevel.h
#)
#
#target_link_libraries(logreader PRIVATE
#        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
#        fmt::fmt
#        oatpp::oatpp
#        nlohmann_json::nlohmann_json
#        PRIVATE mongo::mongocxx_shared
#        PRIVATE mongo::bsoncxx_shared
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
#        oatpp::oatpp-swagger
        oatpp::oatpp-websocket
        nlohmann_json::nlohmann_json
        PRIVATE mongo::mongocxx_shared
        PRIVATE mongo::bsoncxx_shared
        argparse
        e131_service
        CreatureVoicesLib
        gtest_main
        gmock_main
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
