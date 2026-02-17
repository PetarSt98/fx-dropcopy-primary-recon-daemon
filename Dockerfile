# syntax=docker/dockerfile:1

# Base toolchain image
FROM ubuntu:22.04 AS build-base
ARG DEBIAN_FRONTEND=noninteractive

# Avoid transient mirror inconsistencies by skipping backports, which are not required
# for this project.
RUN sed -i '/backports/d' /etc/apt/sources.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        libbsd-dev \
        ninja-build \
        openjdk-17-jdk-headless \
        pkg-config \
        python3 \
        uuid-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Build and install Aeron
FROM build-base AS aeron
ARG AERON_VERSION=1.43.0
WORKDIR /tmp/aeron

RUN git clone --branch ${AERON_VERSION} --depth 1 https://github.com/real-logic/aeron.git . \
    && cmake -S . -B /tmp/aeron-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -Daeron_BUILD_SAMPLES=OFF \
        -Daeron_BUILD_TESTS=OFF \
    && cmake --build /tmp/aeron-build --parallel \
    && cmake --install /tmp/aeron-build --prefix /opt/aeron

# Developer image: builds the project and keeps the build tree for tests/debugging
FROM build-base AS dev
ENV AERON_HOME=/opt/aeron \
    AERON_DIR=/var/tmp/aeron \
    PATH="/opt/aeron/bin:/workspace/build/release:${PATH}" \
    LD_LIBRARY_PATH="/opt/aeron/lib:${LD_LIBRARY_PATH}"

COPY --from=aeron /opt/aeron /opt/aeron
RUN echo "/opt/aeron/lib" > /etc/ld.so.conf.d/aeron.conf && ldconfig

WORKDIR /workspace
COPY . .

RUN cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=${AERON_HOME} \
    && cmake --build build/release

CMD ["bash"]

# Profiling image: extends dev with perf, FlameGraph toolkit, and Perl
# for CPU flame graph generation. Use with privileged: true in compose.
FROM dev AS profiling

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends git perl bc; \
    rm -rf /var/lib/apt/lists/*; \
    \
    # Best-effort perf install across environments (generic, azure, WSL2).
    KREL="$(uname -r)"; \
    echo "Kernel release: $KREL"; \
    apt-get update; \
    ( \
      apt-get install -y --no-install-recommends \
        linux-tools-common \
        "linux-tools-$KREL" \
        "linux-cloud-tools-$KREL" \
      || apt-get install -y --no-install-recommends linux-tools-generic \
      || apt-get install -y --no-install-recommends linux-tools-azure linux-cloud-tools-azure \
      || apt-get install -y --no-install-recommends linux-tools-standard-WSL2 linux-cloud-tools-standard-WSL2 \
      || true \
    ); \
    rm -rf /var/lib/apt/lists/* || true; \
    \
    # If perf exists, show version; if not, continue (build must still succeed).
    if command -v perf >/dev/null 2>&1; then \
      perf --version || true; \
    else \
      echo "NOTE: perf not available for kernel $KREL in this build environment. Image will be built without perf."; \
    fi; \
    \
    git clone --branch v1.0 --depth 1 https://github.com/brendangregg/FlameGraph.git tools/FlameGraph



# Fix CRLF line endings from Windows host and ensure scripts are executable
RUN find scripts/ -name '*.sh' -exec sed -i 's/\r$//' {} + 2>/dev/null; \
    chmod +x scripts/*.sh 2>/dev/null; \
    true

CMD ["bash"]

# Runtime image: minimal footprint for running the daemon
FROM ubuntu:22.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive

# Avoid transient mirror inconsistencies by skipping backports, which are not required
# for this project.
RUN sed -i '/backports/d' /etc/apt/sources.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libstdc++6 \
        libbsd0 \
        openjdk-17-jre-headless \
    && rm -rf /var/lib/apt/lists/*

COPY --from=aeron /opt/aeron /opt/aeron
COPY --from=dev /workspace/build/release/fx_exec_recond /usr/local/bin/
COPY --from=dev /workspace/build/release/fx_ingest_demo /usr/local/bin/
COPY --from=dev /workspace/build/release/fx_aeron_publisher /usr/local/bin/

ENV AERON_DIR=/var/tmp/aeron \
    PATH="/opt/aeron/bin:${PATH}" \
    LD_LIBRARY_PATH="/opt/aeron/lib:${LD_LIBRARY_PATH}"

RUN echo "/opt/aeron/lib" > /etc/ld.so.conf.d/aeron.conf && ldconfig

ENTRYPOINT ["fx_exec_recond"]
CMD ["aeron:udp?endpoint=localhost:20121", "1001", "aeron:udp?endpoint=localhost:20122", "1002"]
