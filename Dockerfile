
FROM debian:bookworm AS build

RUN apt update && apt upgrade -y

RUN apt install -y cmake libssl-dev libsasl2-dev gcc git file \
    pkgconf libbson-dev libpthreadpool-dev libutf8proc-dev \
    libsystemd-dev ninja-build libsdl2-mixer-dev dpkg-dev uuid-dev \
    util-linux libpipewire-0.3-dev libuv1-dev libcurl4-openssl-dev libprotobuf-dev protobuf-compiler



# Build our stuff
RUN mkdir -p /build/creature-server/src
COPY src/ /build/creature-server/src
COPY cmake/ /build/creature-server/cmake
COPY lib/ /build/creature-server/lib
COPY tests/ /build/creature-server/tests
COPY externals/ /build/creature-server/externals
COPY LICENSE README.md CMakeLists.txt build_oatpp.sh /build/creature-server/
RUN ls -lart /build/creature-server/

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
FROM debian:bookworm-slim AS runtime

# Some of our libs need runtime bits
RUN apt update && apt upgrade -y && \
    apt install -y libsasl2-2 libicu72 libsdl2-mixer-2.0-0 flac locales-all libutf8proc2 \
                   libuuid1 util-linux pipewire libuv1 libcurl4 libprotobuf32 && \
    rm -rf /var/lib/apt/lists

RUN mkdir /app
COPY --from=build /build/creature-server/build/creature-server /app/creature-server
COPY --from=build /usr/local/lib /usr/local/lib

EXPOSE 6666

CMD ["/app/creature-server"]



# Small build for creating a deb package
FROM build AS package

# Make a package
RUN mkdir -p /package
RUN cd /build/creature-server/build && cpack -G DEB && cp *.deb /package
