cmake_minimum_required(VERSION 3.25)

project(CreatureServerExternals)

include(ExternalProject)
ExternalProject_Add(
        oatpp
        GIT_REPOSITORY "https://github.com/oatpp/oatpp.git"
        GIT_TAG "1.3.0-latest"
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
)

ExternalProject_Add(
        oatpp-swagger
        GIT_REPOSITORY "https://github.com/oatpp/oatpp-swagger.git"
        GIT_TAG "1.3.0-latest"
        CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
        DEPENDS oatpp
)

