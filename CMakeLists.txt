cmake_minimum_required(VERSION 3.15)


project(creature-server)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

include(FetchContent)
FetchContent_Declare(
        gRPC
        GIT_REPOSITORY https://github.com/grpc/grpc
        GIT_TAG        v1.53.0
)
set(FETCHCONTENT_QUIET OFF)
FetchContent_MakeAvailable(gRPC)

find_package(Protobuf REQUIRED)


# Our source files
file(GLOB serverFiles
        src/server/*
        src/messaging/*
        )

file(GLOB clientFiles
        src/client/*
        src/messaging/*
        )

# Set up our includes
include_directories(
        src/
        ${Protobuf_INCLUDE_DIRS}
)

add_executable(creature-server
        ${serverFiles}
        ${Protobuf_LIBRARIES}
        )

target_link_libraries(creature-server
        PUBLIC
        protobuf::libprotobuf
        grpc++)

add_executable(client
        ${clientFiles}
        ${Protobuf_LIBRARIES}
        )

target_link_libraries(client
        PUBLIC
        protobuf::libprotobuf
        grpc++)
