cmake_minimum_required(VERSION 3.18)


project(creature-server
        VERSION 0.2.0
        DESCRIPTION "Server for April's Creatures"
        HOMEPAGE_URL https://github.com/opsnlops/creature-server
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)

set(ABSL_PROPAGATE_CXX_STD on)

find_package(PkgConfig REQUIRED)

set(CMAKE_POLICY_DEFAULT_CMP0042 NEW)

# This project uses threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
        gRPC
        GIT_REPOSITORY https://github.com/grpc/grpc
        GIT_TAG        v1.53.0
)
set(FETCHCONTENT_QUIET OFF)
FetchContent_MakeAvailable(gRPC)

find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)

# Our source files
file(GLOB SERVER_SRCS
        src/server/*
        src/messaging/*
        )

file(GLOB clientFiles
        src/client/*
        src/messaging/*
        )

file(GLOB mongoTestFiles
        src/mongo-test/*
        src/messaging/*
        )

# Set up our includes
include_directories(
        src/
        ${Protobuf_INCLUDE_DIRS}
        ${LIBMONGOCXX_INCLUDE_DIR}
        ${LIBBSONCXX_INCLUDE_DIR}
)

add_executable(creature-server
        ${SERVER_SRCS}
        ${Protobuf_LIBRARIES}
        ${libmongocxx_LIBRARIES}
        ${libbsoncxx_LIBRARIES}
        )

target_link_libraries(creature-server
        PUBLIC
        protobuf::libprotobuf
        grpc++
        mongo::mongocxx_shared
        mongo::bsoncxx_shared
        )


add_executable(client
        ${clientFiles}
        ${Protobuf_LIBRARIES}
        )

target_link_libraries(client
        PUBLIC
        protobuf::libprotobuf
        grpc++
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
        )