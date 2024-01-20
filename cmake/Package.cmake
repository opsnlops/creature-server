
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# Enable shlibs for this project
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)

set(CPACK_DEBIAN_PACKAGE_NAME "creature-server")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "April's Creature Workshop Server")

set(CPACK_PACKAGE_DESCRIPTION
        "The 'Creature Server' for April's Creature Workshop. Offers
        a gRPC based service for playing and managing creatures."
        )

set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# Pull the version from the project
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# Use a timestamp for the release number
string(TIMESTAMP NOW "%s")
set(CPACK_DEBIAN_PACKAGE_RELEASE ${NOW})

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "April White <april@opsnlops.io>")

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsasl2-2, libicu72, libsdl2-mixer-2.0-0, flac, locales-all")

include(CPack)