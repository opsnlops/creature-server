
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

# "/" rather than "/usr" so install() can reach /etc and /var, which the config
# file needs and which the /usr prefix made unreachable — an install()
# DESTINATION of "/etc/..." under a "/usr" prefix is packaged as "/usr/etc/...".
# Every destination is therefore spelled relative and starts with usr/ where it
# belongs there (issue #237).
set(CPACK_PACKAGING_INSTALL_PREFIX "/")

# Pull the version from the project
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# Use a timestamp for the release number
string(TIMESTAMP NOW "%s")
set(CPACK_DEBIAN_PACKAGE_RELEASE ${NOW})

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "April White <april@opsnlops.io>")

# Maintainer scripts: create the creature-server system user and
# /var/cache/creature-server on install, and remove the cache on purge
# (issue #235). That is the FHS and Debian Policy location for
# regenerable application data, which is exactly what the Opus packet cache is.
#
# These have to be maintainer scripts rather than install() calls, because
# CPACK_PACKAGING_INSTALL_PREFIX below is /usr: an install() DESTINATION of
# "/var/cache/..." would be packaged as "/usr/var/cache/...".
#
# They must stay executable in git, or dpkg refuses them.
#
# `conffiles` marks the two files under /etc as configuration, so dpkg preserves
# local edits across upgrades and prompts on conflict. Built with raw CPack
# rather than debhelper, nothing infers this from the path, so it is listed
# explicitly — and without it an upgrade would silently overwrite the operator's
# config, which is the one thing a config file must never do (issue #237).
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/scripts/debian/postrm;${CMAKE_CURRENT_SOURCE_DIR}/scripts/debian/conffiles")

# The packaged configuration. Version-controlled in this repo so the production
# settings live with the code rather than only in a unit file, matching how
# creature-world ships /etc/creature/world.json from the Swift monorepo.
#
# Secrets are NOT here: see scripts/debian/default for why.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/scripts/debian/server.json
        DESTINATION etc/creature
        COMPONENT creature-server)
# 0600, not the default 0644. This file holds the ElevenLabs and Honeycomb keys,
# and shipping it world-readable would make moving them out of the unit
# pointless — the whole reason for this file is that unit files under
# /lib/systemd/system are readable by every user on the box.
#
# root-only is still correct for a unit running as a non-root User=, because
# systemd reads EnvironmentFile= as root before dropping privileges.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/scripts/debian/default
        DESTINATION etc/default
        RENAME creature-server
        PERMISSIONS OWNER_READ OWNER_WRITE
        COMPONENT creature-server)

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# We can't have the (very out of date) Mongo drivers from Debian on the host
set(CPACK_DEBIAN_PACKAGE_CONFLICTS "libmongoclient0, libmongoc-dev, libmongoc-1.0-0, libmongoclient-dev, libbson-1.0-0, libbson-dev")
# Note: CPACK_DEBIAN_PACKAGE_SHLIBDEPS is ON, so all linked libraries are auto-detected.
# Only list runtime-only dependencies here (not direct library links).
# - locales-all: For proper locale support
# - adduser: postinst creates the creature-server system user (Policy 9.2.2
#            requires depending on it rather than assuming it is present)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "locales-all, adduser")

# rhubarb-lip-sync is optional: only needed when --lip-sync-engine=rhubarb (legacy fallback)
# The default engine is now whisper.cpp (bundled in the package)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "rhubarb-lip-sync")

if(TRUE) # uWebSockets license notices ship with every package
    install(FILES ${uwebsockets_SOURCE_DIR}/LICENSE
            DESTINATION usr/share/doc/creature-server
            RENAME uWebSockets.LICENSE
            COMPONENT creature-server)
    install(FILES ${uwebsockets_SOURCE_DIR}/uSockets/LICENSE
            DESTINATION usr/share/doc/creature-server
            RENAME uSockets.LICENSE
            COMPONENT creature-server)
endif()

include(CPack)
