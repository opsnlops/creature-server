
FROM debian:trixie AS build

RUN apt update && apt upgrade -y

RUN apt install -y \
        cmake \
        dpkg-dev \
        file \
        clang-19 \
        git \
        libbson-dev \
        libcurl4-openssl-dev \
        libpipewire-0.3-dev \
        libprotobuf-dev \
        libpthreadpool-dev \
        libsasl2-dev \
        libsdl2-mixer-dev \
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

# CMakeLists.txt's top-level configure_file() needs this template at configure
# time (see CMakeLists.txt:27). Pulled in by itself in Phase 1 so we don't have
# to COPY src/ wholesale just for one file — that'd defeat the cache split.
COPY src/server/Version.h.in /build/creature-server/src/server/Version.h.in

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
RUN cd /build/creature-server/build && ninja -j4 deps_only

# ---- Phase 2: copy our source + build the final binary. Only this layer
# re-runs on a typical "I changed a .cpp file" PR.

COPY src/ /build/creature-server/src
COPY tests/ /build/creature-server/tests

# Re-run cmake configure so the file(GLOB) source-list calls re-evaluate
# against the now-populated src/ tree. (Without this, ninja would still see
# the empty-src configuration from Phase 1.) Configure is fast — a few seconds
# — once the FetchContent sources are already populated.
RUN cd /build/creature-server/build && cmake .. && ninja -j4


# Now build a small runtime
FROM debian:trixie-slim AS runtime

# Runtime dependencies
# Note: List only top-level packages; apt pulls in their dependencies automatically
RUN apt update && apt upgrade -y && \
    apt install -y \
        libcurl4 \
        libprotobuf32 \
        libsasl2-2 \
        libsdl2-mixer-2.0-0 \
        libssl3 \
        libuuid1 \
        libzstd1 \
        locales-all \
        pipewire && \
    rm -rf /var/lib/apt/lists

RUN mkdir /app
COPY --from=build /build/creature-server/build/creature-server /app/creature-server
COPY --from=build /usr/local/lib /usr/local/lib

# Whisper model and CMU dictionary for lip sync
RUN mkdir -p /usr/share/creature-server/data
COPY --from=build /build/creature-server/build/data/ggml-base.en.bin /usr/share/creature-server/data/
COPY --from=build /build/creature-server/build/data/cmudict.dict /usr/share/creature-server/data/

EXPOSE 8000

CMD ["/app/creature-server"]



# Small build for creating a deb package
FROM build AS package

# Make a package
RUN mkdir -p /package
RUN cd /build/creature-server/build && cpack -G DEB && cp *.deb /package
