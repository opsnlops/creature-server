cmake_minimum_required(VERSION 3.25)


project(e131_service)

set(CMAKE_CXX_STANDARD 20)

# Why are UUIDs so hard in 2024
if(NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    pkg_search_module(UUID REQUIRED uuid)
endif()


# Create a static library out of libe131
add_library(libe131 STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../libe131/src/e131.c
)


# Define our library
add_library(e131_service STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/E131Server.cpp
        src/Universe.cpp
        src/Universe.h
)

# Include directories for this library
target_include_directories(e131_service PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../libe131/src
)

target_link_libraries(e131_service
        libe131
        spdlog::spdlog $<$<BOOL:${MINGW}>:ws2_32>
        fmt::fmt
        ${UUID_LIBRARIES}
)