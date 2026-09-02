
FROM debian:trixie AS build

RUN apt update && apt upgrade -y

# apt update again in the same layer as install so the package index is always
# fresh right before the install resolves versions. Without this, a cached update
# layer can pin a point-release version that Debian has since removed from the pool
# (e.g. an openssl security bump), making apt install 404 with exit 100.
RUN apt update && apt install -y \
        cmake \
        dpkg-dev \
        file \
        clang-19 \
        git \
        libbson-dev \
        libcurl4-openssl-dev \
        libasound2-dev \
        libmp3lame-dev \
        libprotobuf-dev \
        libpthreadpool-dev \
        libsasl2-dev \
        libssl-dev \
        libsystemd-dev \
        libutf8proc-dev \
        libuv1-dev \
        ninja-build \
        pkgconf \
        protobuf-compiler \
        util-linux \
        uuid-dev

# Use the clang-19 we installed above (without this, CMake picks Debian's default gcc)
ENV CC=clang-19
ENV CXX=clang++-19

# ---- Phase 1: build oatpp + all FetchContent dependencies in a layer that
# only invalidates when CMakeLists.txt / cmake/ / lib/ / externals/ / the
# oatpp build script change. The heavy compile (Mongo C/C++ driver, OTel,
# whisper, opus, uvgrtp, googletest, fmt, spdlog) lands here and gets reused
# by every subsequent build that doesn't change those inputs. See issue #13.

RUN mkdir -p /build/creature-server
COPY cmake/ /build/creature-server/cmake
COPY lib/ /build/creature-server/lib
COPY externals/ /build/creature-server/externals
COPY LICENSE README.md CMakeLists.txt build_oatpp.sh /build/creature-server/
COPY docs/transport-route-manifest.json /build/creature-server/docs/transport-route-manifest.json
COPY scripts/transport-route-manifest.py /build/creature-server/scripts/transport-route-manifest.py

# CMakeLists.txt's top-level configure_file() needs this template at configure
# time (see CMakeLists.txt:27). Pulled in by itself in Phase 1 so we don't have
# to COPY src/ wholesale just for one file — that'd defeat the cache split.
COPY src/server/Version.h.in /build/creature-server/src/server/Version.h.in

# Note: VERSION.txt is deliberately NOT COPYed in Phase 1 — that'd invalidate
# this layer (and force a deps_only rebuild) on every release. CMakeLists.txt
# falls back to "0.0.0" when VERSION.txt is absent; the real version is
# applied in Phase 2 after the deps cache is locked in. See issue #18.

# Clone the base64 lib if not already present (lib/base64 might be in-tree
# or might need fetching depending on how the workspace was set up).
RUN if [ ! -f /build/creature-server/lib/base64/include/base64.hpp ]; then \
        git clone https://github.com/tobiaslocker/base64.git /build/creature-server/lib/base64; \
    fi

# Build oatpp into externals/install.
RUN cd /build/creature-server/ && ./build_oatpp.sh

# Configure CMake. file(GLOB serverFiles src/...) returns an empty list at
# this stage (src/ doesn't exist yet) but configure succeeds — add_executable
# doesn't check source-file existence, only the build does. We're not going
# to build the executable in this layer.
RUN cd /build/creature-server && \
    mkdir build && cd build && \
    cmake -DCMAKE_MAKE_PROGRAM=ninja -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          ..

# Pre-compile every heavy FetchContent dep via the deps_only umbrella target
# (defined in CMakeLists.txt). This is the ~15 minute step today; with this
# layer cached it only re-runs when CMakeLists / cmake / lib / externals /
# build_oatpp.sh change.
RUN cd /build/creature-server/build && ninja -j8 deps_only

# ---- Phase 2: copy our source + build the final binary. Only this layer
# re-runs on a typical "I changed a .cpp file" PR.

COPY src/ /build/creature-server/src
COPY tests/ /build/creature-server/tests
COPY run_linux_tests.sh /build/creature-server/run_linux_tests.sh

# Real project version goes here, AFTER the deps_only layer is sealed. The
# Phase 2 cmake reconfigure below re-reads VERSION.txt and applies it to
# project() / Version.h / the .deb metadata. See issue #18.
COPY VERSION.txt /build/creature-server/

# Re-run cmake configure so the file(GLOB) source-list calls re-evaluate
# against the now-populated src/ tree. (Without this, ninja would still see
# the empty-src configuration from Phase 1.) Configure is fast — a few seconds
# — once the FetchContent sources are already populated.
RUN cd /build/creature-server/build && cmake .. && ninja -j8

# CPack package export for Linux validation from a macOS host. This is a build
# artifact stage, not a runtime image; production installs the resulting .deb.
FROM build AS package

# The .deb declares this runtime dependency. Install it in the validation stage
# so the extracted payload runs under the same en_US.UTF-8 locale contract as
# an installed package, rather than under the intentionally minimal builder.
RUN apt update && apt install -y --no-install-recommends locales-all

# Make a package
RUN mkdir -p /package
RUN cd /build/creature-server/build && cpack -G DEB && cp *.deb /package

# Validate the exact filesystem payload that will be deployed. The smoke gate
# extracts the package, checks its runtime links and notices, then boots the
# packaged binary with default uWebSockets and explicit oat++ rollback.
RUN python3 /build/creature-server/tests/transport/debian_package_smoke_test.py \
        --package /package/creature-server_*.deb \
        --network-device lo
