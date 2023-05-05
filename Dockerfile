

# docker buildx build --platform linux/amd64,linux/arm64

FROM debian:bookworm as build

RUN apt update && apt upgrade -y

RUN apt install -y cmake libssl-dev libsasl2-dev clang git \
    libprotobuf-dev pkgconf libmongoclient-dev libbson-dev libpthreadpool-dev  \
    libsystemd-dev ninja-build libspdlog-dev libsdl2-mixer-dev


# Install the latest Mongo driver
RUN mkdir -p /build/mongo
ADD https://github.com/mongodb/mongo-c-driver/releases/download/1.23.3/mongo-c-driver-1.23.3.tar.gz /build/mongo/c-driver.tar.gz
ADD https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.7.1/mongo-cxx-driver-r3.7.1.tar.gz /build/mongo/cxx-driver.tar.gz
RUN cd /build/mongo && tar -xzvf c-driver.tar.gz && tar -xzvf cxx-driver.tar.gz

RUN cd /build/mongo/mongo-c-driver-1.23.3/build && \
    cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_MAKE_PROGRAM=ninja -G Ninja .. && \
    ninja && \
    ninja install

RUN cd /build/mongo/mongo-cxx-driver-r3.7.1/build && \
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
    cmake -DCMAKE_MAKE_PROGRAM=ninja -G Ninja .. && \
    ninja


# Now build a small runtime
FROM debian:bookworm-slim as runtime

# Some of our libs need runtime bits
RUN apt update && apt upgrade -y && \
    apt install -y libsasl2-2 libicu72 libspdlog1.10 libprotobuf32 libsdl2-mixer-2.0-0 locales-all && \
    rm -rf /var/lib/apt/lists

RUN mkdir /app
COPY --from=build /build/creature-server/build/creature-server /app/creature-server
COPY --from=build /build/creature-server/build/mongo-test /app/mongo-test
COPY --from=build /usr/local/lib /usr/local/lib

EXPOSE 6666

CMD ["/app/creature-server"]
