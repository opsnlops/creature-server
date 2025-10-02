
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



# Build our stuff
RUN mkdir -p /build/creature-server/src
COPY src/ /build/creature-server/src
COPY cmake/ /build/creature-server/cmake
COPY lib/ /build/creature-server/lib
COPY tests/ /build/creature-server/tests
COPY externals/ /build/creature-server/externals
COPY LICENSE README.md CMakeLists.txt build_oatpp.sh /build/creature-server/
RUN ls -lart /build/creature-server/

# Clone missing git submodule (base64)
RUN git clone https://github.com/tobiaslocker/base64.git /build/creature-server/lib/base64

# Install the externals
RUN cd /build/creature-server/ && ./build_oatpp.sh

# Build the server
RUN cd /build/creature-server && \
    mkdir build && \
    cd build && \
    cmake -DCMAKE_MAKE_PROGRAM=ninja -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          ..

# Run the build in a different layer so we avoid grabbing the code over
# and over again
RUN cd /build/creature-server/build && ninja


# Now build a small runtime
FROM debian:trixie-slim AS runtime

# Some of our libs need runtime bits
RUN apt update && apt upgrade -y && \
    apt install -y \
        flac \
        libcurl4 \
        libicu72 \
        libprotobuf32 \
        libsasl2-2 \
        libsdl2-mixer-2.0-0 \
        libssl3 \
        libuuid1 \
        libutf8proc2 \
        libuv1 \
        locales-all \
        pipewire \
        util-linux && \
    rm -rf /var/lib/apt/lists

RUN mkdir /app
COPY --from=build /build/creature-server/build/creature-server /app/creature-server
COPY --from=build /usr/local/lib /usr/local/lib

EXPOSE 8000

CMD ["/app/creature-server"]



# Small build for creating a deb package
FROM build AS package

# Make a package
RUN mkdir -p /package
RUN cd /build/creature-server/build && cpack -G DEB && cp *.deb /package
