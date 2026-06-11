# Multi-stage build: compile against Boost+OpenSSL, ship a slim runtime image.
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake \
        libboost-system-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j "$(nproc)"

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        libboost-system1.83.0 libssl3t64 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/eh-loadtest /usr/local/bin/eh-loadtest

# A single pod holding thousands of sockets needs a high fd limit. Set this in
# the container runtime / pod securityContext too (see k8s/deployment.yaml).
ENTRYPOINT ["/usr/local/bin/eh-loadtest"]
