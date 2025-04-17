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

# Copy the compiled binary from the build stage
COPY --from=build /painters_server /painters_server

EXPOSE 591

# Run your app
CMD ["./painters_server"]