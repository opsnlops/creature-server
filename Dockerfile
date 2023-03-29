

# docker buildx build --platform linux/amd64,linux/arm64

FROM debian:latest as build

RUN apt update && apt upgrade -y

RUN apt install cmake libssl-dev libsasl2-dev g++ git \
    libprotobuf-dev pkgconf libmongoclient-dev libbson-dev libpthreadpool-dev libsystemd-dev \
    ninja-build \
    -y

# Install the latest Mongo driver
RUN mkdir -p /build/mongo
ADD https://github.com/mongodb/mongo-c-driver/releases/download/1.23.2/mongo-c-driver-1.23.2.tar.gz /build/mongo/c-driver.tar.gz
ADD https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.7.1/mongo-cxx-driver-r3.7.1.tar.gz /build/mongo/cxx-driver.tar.gz
RUN cd /build/mongo && tar -xzvf c-driver.tar.gz && tar -xzvf cxx-driver.tar.gz

RUN cd /build/mongo/mongo-c-driver-1.23.2/build && \
    cmake -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local .. && \
    make -j 8 && \
    make install

RUN cd /build/mongo/mongo-cxx-driver-r3.7.1/build && \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local .. && \
    make -j 8 && \
    make install


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


CMD ["/build/creature-server/build/creature-server"]

