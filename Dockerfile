

# docker buildx build --platform linux/amd64,linux/arm64

FROM debian:bookworm as build

RUN apt update && apt upgrade -y

RUN apt install -y cmake libssl-dev libsasl2-dev clang git \
    pkgconf libbson-dev libpthreadpool-dev  \
    libsystemd-dev ninja-build libsdl2-mixer-dev

RUN update-alternatives --install /usr/bin/cc cc /usr/bin/clang 100 && \
    update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang 100

# Install the latest Mongo driver
RUN mkdir -p /build/mongo
ADD https://github.com/mongodb/mongo-c-driver/releases/download/1.24.3/mongo-c-driver-1.24.3.tar.gz /build/mongo/c-driver.tar.gz
ADD https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.8.0/mongo-cxx-driver-r3.8.0.tar.gz /build/mongo/cxx-driver.tar.gz
RUN cd /build/mongo && tar -xzvf c-driver.tar.gz && tar -xzvf cxx-driver.tar.gz

RUN cd /build/mongo/mongo-c-driver-1.24.3/build && \
    cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_MAKE_PROGRAM=ninja -G Ninja .. && \
    ninja && \
    ninja install

RUN cd /build/mongo/mongo-cxx-driver-r3.8.0/build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_MAKE_PROGRAM=ninja -G Ninja .. && \
    ninja && \
    ninja install


# Build our stuff
RUN mkdir -p /build/creature-server/src /build/creature-server/messaging
COPY src/ /build/creature-server/src
COPY messaging/ /build/creature-server/messaging
COPY CMakeLists.txt /build/creature-server/
RUN ls -lart /build/creature-server/
RUN cd /build/creature-server && \
    mkdir build && \
    cd build && \
    cmake -DCMAKE_MAKE_PROGRAM=ninja -G Ninja \
          -DgRPC_ABSL_PROVIDER=module \
          -DgRPC_CARES_PROVIDER=module \
          -DgRPC_PROTOBUF_PROVIDER=module \
          -DgRPC_RE2_PROVIDER=module \
          -DgRPC_SSL_PROVIDER=module \
          -DgRPC_ZLIB_PROVIDER=module \
          -DgRPC_INSTALL=ON \
          -DCMAKE_BUILD_TYPE=Release \
          .. && \
    ninja


# Now build a small runtime
FROM debian:bookworm-slim as runtime

# Some of our libs need runtime bits
RUN apt update && apt upgrade -y && \
    apt install -y libsasl2-2 libicu72 libsdl2-mixer-2.0-0 flac locales-all && \
    rm -rf /var/lib/apt/lists

RUN mkdir /app
COPY --from=build /build/creature-server/build/creature-server /app/creature-server
COPY --from=build /usr/local/lib /usr/local/lib

EXPOSE 6666

CMD ["/app/creature-server"]
