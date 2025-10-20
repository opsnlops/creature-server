
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# Enable shlibs for this project
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)

set(CPACK_DEBIAN_PACKAGE_NAME "creature-server")
set(CPACK_DEBIAN_PACKAGE_SECTION "electronics")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "April's Creature Workshop Server")

set(CPACK_PACKAGE_DESCRIPTION
        "The 'Creature Server' for April's Creature Workshop. Offers
        a websocket based service for playing and managing creatures."
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

# We can't have the (very out of date) Mongo drivers from Debian on the host
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "libmongoclient0, libmongoc-dev, libmongoc-1.0-0, libmongoclient-dev, libbson-1.0-0, libbson-dev")
# Note: CPACK_DEBIAN_PACKAGE_SHLIBDEPS is ON, so all linked libraries are auto-detected.
# Only list runtime-only dependencies here (not direct library links).
# - pipewire: Audio server (runtime, not a library dependency)
# - locales-all: For proper locale support
# - rhubarb-lip-sync: For generating lip sync data from WAV files
# - ffmpeg: For converting MP3 to WAV (temporary until upgrading to ElevenLabs Pro)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "pipewire, locales-all, rhubarb-lip-sync, ffmpeg")

install(DIRECTORY ${CMAKE_SOURCE_DIR}/externals/build/oatpp-swagger-prefix/src/oatpp-swagger/res/
        DESTINATION /usr/share/creature-server/swagger-ui
)

include(CPack)
