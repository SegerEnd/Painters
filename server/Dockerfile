FROM ubuntu:25.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies for building
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    zlib1g-dev \
    g++ \
    libuv1-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone and build uWebSockets
RUN git clone https://github.com/uNetworking/uWebSockets.git \
    && cd uWebSockets \
    && git submodule update --init --depth 1 \
    && make -C uSockets \
    && make

# Copy the source code, .txt otherwise ufbt wants to build it too
COPY main.cpp .

# Compile app with uWebSockets headers and library
RUN g++ -std=c++23 -IuWebSockets/src -IuWebSockets/uSockets/src -o painters_server main.cpp \
    uWebSockets/uSockets/uSockets.a -lpthread -lz -luv -lssl -lcrypto

# Runtime stage
FROM ubuntu:latest

ENV TZ=Europe/Amsterdam
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -yq tzdata && \
    ln -fs /usr/share/zoneinfo/$TZ /etc/localtime && \
    dpkg-reconfigure -f noninteractive tzdata

# Copy the compiled binary from the build stage
COPY --from=build /painters_server app/painters_server

EXPOSE 80

# Remove downloaded packages and clean up image
RUN apt-get clean && \
    rm -rf /var/lib/apt/lists/* && \
    apt-get autoremove -y && \
    rm -rf /var/cache/apt/archives && \
    rm -rf /var/cache/apt/archives/lock && \
    rm -rf /var/lib/dpkg/lock && \
    rm -rf /var/lib/dpkg/lock-frontend && \
    rm -rf /var/lib/apt/lists/* && \
    rm -rf /tmp/* && \
    rm -rf /var/tmp/*

WORKDIR /app

# Run your app
CMD ["./painters_server"]