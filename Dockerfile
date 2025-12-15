# syntax=docker/dockerfile:1

# Build Aeron and project binaries
FROM ubuntu:22.04 AS aeron-build
ARG DEBIAN_FRONTEND=noninteractive
ARG AERON_VERSION=1.43.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        curl \
        ca-certificates \
        pkg-config \
        git \
        openjdk-11-jdk-headless \
        zlib1g-dev \
        uuid-dev \
        libbsd-dev \
        python3 \
    && rm -rf /var/lib/apt/lists/*

# Fetch and build Aeron C++ client + media driver (no samples/tests)
RUN git clone --branch ${AERON_VERSION} --depth 1 https://github.com/real-logic/aeron.git /tmp/aeron \
    && cmake -S /tmp/aeron -B /tmp/aeron-build \
        -DCMAKE_BUILD_TYPE=Release \
        -Daeron_BUILD_SAMPLES=OFF \
        -Daeron_BUILD_TESTS=OFF \
    && cmake --build /tmp/aeron-build --parallel \
    && cmake --install /tmp/aeron-build

FROM ubuntu:22.04 AS builder
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        openjdk-11-jre-headless \
    && rm -rf /var/lib/apt/lists/*

COPY --from=aeron-build /usr/local /usr/local
RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/aeron.conf && ldconfig

WORKDIR /workspace
ENV AERON_ROOT=/usr/local
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local \
    && cmake --build build --target fx_exec_recond fx_ingest_demo

FROM ubuntu:22.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libstdc++6 \
        openjdk-11-jre-headless \
    && rm -rf /var/lib/apt/lists/*

COPY --from=aeron-build /usr/local /usr/local
COPY --from=builder /workspace/build/fx_exec_recond /usr/local/bin/fx_exec_recond
COPY --from=builder /workspace/build/fx_ingest_demo /usr/local/bin/fx_ingest_demo
RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/aeron.conf && ldconfig

ENV AERON_DIR=/var/tmp/aeron
ENTRYPOINT ["/usr/local/bin/fx_exec_recond"]
CMD ["aeron:udp?endpoint=localhost:20121", "1001", "aeron:udp?endpoint=localhost:20122", "1002"]
