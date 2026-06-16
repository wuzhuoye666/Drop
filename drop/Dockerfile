# Multi-stage build for drop_agent (C++ with eBPF support)
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ca-certificates \
    libgrpc-dev libgrpc++-dev protobuf-compiler libprotobuf-dev \
    protobuf-compiler-grpc \
    libgflags-dev libgoogle-glog-dev \
    libpq-dev \
    libbpf-dev clang-17 llvm-17 \
    linux-headers-generic \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/drop

# Copy source
COPY drop/ ./drop/
COPY Makefile ./

# Copy pre-downloaded FlameGraph (avoids git clone network issues in Docker)
COPY third_party/FlameGraph/ /opt/FlameGraph/

# Build C++ binaries (clean build to avoid stale CMakeCache)
RUN cd drop && rm -rf build && mkdir -p build && cd build && \
    cmake .. && make -j$(nproc)

# Build eBPF objects
RUN cd drop/common/bpf && make

# ──────────────────────────────────────────────────────────────────
# Runtime image — include -dev packages for shared libs not in slim repo
# This is not minimal but guarantees working .so resolution.
# TODO(Phase9): switch to static linking or extract only .so files.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++-dev libgrpc-dev libprotobuf-dev \
    libgoogle-glog-dev libpq-dev libbpf-dev \
    curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /opt/FlameGraph /opt/FlameGraph

COPY --from=builder /src/drop/drop/build/drop_agent /opt/drop/drop_agent
COPY --from=builder /src/drop/drop/build/drop_server /opt/drop/drop_server
COPY --from=builder /src/drop/drop/common/bpf/offcpu.bpf.o /opt/drop/bpf/offcpu.bpf.o

# Ensure BPF filesystem is mountable
RUN mkdir -p /sys/fs/bpf /sys/kernel/debug

WORKDIR /opt/drop
