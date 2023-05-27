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

find_package(PkgConfig REQUIRED)

set(CMAKE_POLICY_DEFAULT_CMP0042 NEW)

# This project uses threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Abseil
FetchContent_Declare(
  absl
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG master
)
FetchContent_MakeAvailable(absl)

# fmt
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt
    GIT_TAG 9.1.0
)
FetchContent_MakeAvailable(fmt)

# fmt
set(FMT_HEADER_ONLY ON)
set(FMT_LOCALE ON)

# spdlog
#find_package(spdlog REQUIRED)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog
    GIT_TAG v1.11.0
)
FetchContent_MakeAvailable(spdlog)

# Protobuf
find_package(Protobuf REQUIRED)
message(STATUS "Using Protocol Buffers ${protobuf_VERSION}")

# gRPC
FetchContent_Declare(
        gRPC
        GIT_REPOSITORY https://github.com/grpc/grpc
        GIT_TAG        v1.54.2
)
set(FETCHCONTENT_QUIET OFF)
FetchContent_MakeAvailable(gRPC)

# MongoDB
find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)

# SDL
find_package(SDL2 REQUIRED)
find_package(SDL2_mixer REQUIRED)



# Our source files
file(GLOB SERVER_SRCS
        src/exception/*
        src/messaging/*
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
        src/messaging/*
        )

file(GLOB mongoTestFiles
        src/mongo-test/*
        )

file(GLOB logReaderFiles
        src/client/*.h
        src/exception/*
        src/messaging/*
        src/log-monitor/*
        )

# Set up our includes
include_directories(
        src/
        ${Protobuf_INCLUDE_DIRS}
        ${LIBMONGOCXX_INCLUDE_DIR}
        ${LIBBSONCXX_INCLUDE_DIR}
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_MIXER_INCLUDE_DIRS}
)

add_executable(creature-server
        ${SERVER_SRCS}
        ${Protobuf_LIBRARIES}
        ${libmongocxx_LIBRARIES}
        )

target_link_libraries(creature-server
        PUBLIC
        protobuf::libprotobuf
        grpc++
        mongo::mongocxx_shared
        mongo::bsoncxx_shared
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        SDL2::SDL2
        SDL2_mixer::SDL2_mixer
        absl::flags
        )


add_executable(client
        ${clientFiles}
        )

target_link_libraries(client
        PUBLIC
        protobuf::libprotobuf
        grpc++
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        ${PROTOBUF_LIBRARIES}
        fmt::fmt
        mongo::bsoncxx_shared
        )

add_executable(logreader
        ${logReaderFiles}
        )

target_link_libraries(logreader
        PUBLIC
        protobuf::libprotobuf
        grpc++
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        ${PROTOBUF_LIBRARIES}
        fmt::fmt
        mongo::bsoncxx_shared
        )

add_executable(mongo-test
        ${mongoTestFiles}
        ${Protobuf_LIBRARIES}
        ${libmongocxx_LIBRARIES}
        ${libbsoncxx_LIBRARIES}
        )

target_link_libraries(mongo-test
        PUBLIC
        protobuf::libprotobuf
        grpc++
        mongo::mongocxx_shared
        mongo::bsoncxx_shared
        Threads::Threads
        ${PROTOBUF_LIBRARIES}
        )
