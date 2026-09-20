cmake_minimum_required(VERSION 3.20)

project(creature_server_uwebsockets_spike LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
find_package(Threads REQUIRED)

if(NOT TARGET uSockets)
    FetchContent_Declare(
        uwebsockets
        GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
        GIT_TAG v20.79.0
        GIT_SHALLOW TRUE
        GIT_SUBMODULES uSockets
        GIT_SUBMODULES_RECURSE FALSE
    )
    FetchContent_MakeAvailable(uwebsockets)

    file(
        GLOB USOCKETS_SOURCES CONFIGURE_DEPENDS
        "${uwebsockets_SOURCE_DIR}/uSockets/src/*.c"
        "${uwebsockets_SOURCE_DIR}/uSockets/src/eventing/*.c"
        "${uwebsockets_SOURCE_DIR}/uSockets/src/crypto/*.c"
        "${uwebsockets_SOURCE_DIR}/uSockets/src/io_uring/*.c"
    )
    add_library(uSockets STATIC ${USOCKETS_SOURCES})
    target_include_directories(uSockets PUBLIC "${uwebsockets_SOURCE_DIR}/uSockets/src")
    target_compile_definitions(uSockets PRIVATE LIBUS_NO_SSL)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_executable(creature-server-uwebsockets-spike main.cpp)
target_include_directories(creature-server-uwebsockets-spike PRIVATE "${uwebsockets_SOURCE_DIR}/src")
target_compile_definitions(creature-server-uwebsockets-spike PRIVATE UWS_NO_ZLIB)
target_link_libraries(
    creature-server-uwebsockets-spike
    PRIVATE uSockets nlohmann_json::nlohmann_json Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        creature-server-uwebsockets-spike
        PRIVATE -Wall -Wextra -Wpedantic -Wshadow
    )
endif()

if(CREATURE_BUILD_UWS_OTEL_GATE)
    add_executable(
        creature-server-uwebsockets-otel-gate
        main.cpp
        otel_gate_globals.cpp
        ${CMAKE_SOURCE_DIR}/src/server/database.cpp
        ${CMAKE_SOURCE_DIR}/src/server/fixture/get.cpp
        ${CMAKE_SOURCE_DIR}/src/server/fixture/getall.cpp
        ${CMAKE_SOURCE_DIR}/src/server/fixture/helpers.cpp
        ${CMAKE_SOURCE_DIR}/src/server/metrics/counters.cpp
        ${CMAKE_SOURCE_DIR}/src/server/sensors/SensorDataCache.cpp
        ${CMAKE_SOURCE_DIR}/src/server/ws/service/DmxFixtureReadService.cpp
        ${CMAKE_SOURCE_DIR}/src/model/DmxFixture.cpp
        ${CMAKE_SOURCE_DIR}/src/util/JsonParser.cpp
        ${CMAKE_SOURCE_DIR}/src/util/ObservabilityManager.cpp
        ${CMAKE_SOURCE_DIR}/src/util/Result.cpp
        ${CMAKE_SOURCE_DIR}/src/util/helpers.cpp
    )
    target_include_directories(
        creature-server-uwebsockets-otel-gate
        PRIVATE
            "${uwebsockets_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_BINARY_DIR}"
            "${MOODYCAMEL_DIR}"
            "${LIBBASE64_DIR}/include"
    )
    target_compile_definitions(
        creature-server-uwebsockets-otel-gate
        PRIVATE CREATURE_UWS_OTEL_GATE UWS_NO_ZLIB
    )
    target_link_libraries(
        creature-server-uwebsockets-otel-gate
        PRIVATE
            uSockets nlohmann_json::nlohmann_json Threads::Threads
            spdlog::spdlog fmt::fmt mongocxx::mongocxx bsoncxx::bsoncxx
            opentelemetry_trace opentelemetry_exporter_otlp_http opentelemetry_resources
            opentelemetry_common opentelemetry_http_client_curl opentelemetry_metrics
            opentelemetry_exporter_otlp_http_metric CURL::libcurl OpenSSL::SSL OpenSSL::Crypto
            ${MONGO_SYSTEM_LIBS}
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            creature-server-uwebsockets-otel-gate
            PRIVATE -Wall -Wextra -Wpedantic -Wshadow
        )
    endif()
endif()
