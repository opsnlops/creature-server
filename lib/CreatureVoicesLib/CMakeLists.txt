cmake_minimum_required(VERSION 3.25)


project(CreatureVoicesLib
        VERSION "0.2.0"
        DESCRIPTION "Voice library for April's Creatures"
        HOMEPAGE_URL https://github.com/opsnlops/creature-server/lib/CreatureVoicesLib
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

# Overshadowed declarations keep biting me in the butt 😅
add_compile_options(-Wshadow)
add_compile_options(-Wall)
add_compile_options(-Wextra)
add_compile_options(-Wpedantic)

set(PACKAGE_AUTHOR "April White")


# Log loudly
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# This project uses threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)


find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)

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


# json
FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)


# Use the oatpp that we've already made
find_package(oatpp           REQUIRED PATHS ${CMAKE_SOURCE_DIR}/../../externals/install/lib/cmake/oatpp-1.3.0)

# Define our library
add_library(CreatureVoicesLib STATIC

        src/model/CreatureSpeechRequest.h
        src/model/CreatureSpeechRequest.cpp
        src/model/CreatureSpeechResponse.h
        src/model/CreatureSpeechResponse.cpp
        src/model/HttpMethod.h
        src/model/HttpMethod.cpp
        src/model/Subscription.h
        src/model/Subscription.cpp
        src/model/Voice.h
        src/model/Voice.cpp


        src/CreatureVoices.h
        src/VoiceResult.h
        src/CurlBase.h
        src/CurlBase.cpp
        src/CurlHandle.h
        src/CurlHandle.cpp


        src/methods/CreatureVoices.cpp
        src/methods/generateCreatureSpeech.cpp
        src/methods/getSubscriptionStatus.cpp
        src/methods/listAllAvailableVoices.cpp


)

# Include directories for this library
target_include_directories(CreatureVoicesLib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CURL_INCLUDE_DIR}
        ${oatpp_INCLUDE_DIRS}
)

target_link_libraries(CreatureVoicesLib
        ${CURL_LIBRARIES}
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        nlohmann_json::nlohmann_json
)
