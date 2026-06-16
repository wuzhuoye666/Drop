# Multi-stage build for drop_agent (C++ with eBPF support)
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git \
    libgrpc-dev libgrpc++-dev protobuf-compiler libprotobuf-dev \
    libgflags-dev libgoogle-glog-dev \
    libpq-dev \
    libbpf-dev clang-17 llvm-17 \
    linux-headers-generic \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src/drop

# Copy source
COPY drop/ ./drop/
COPY Makefile ./

# Build
RUN cd drop && mkdir -p build && cd build && \
    cmake .. && make -j$(nproc)

# Build eBPF objects
RUN cd drop/common/bpf && make

# Runtime image
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc17 libprotobuf32 libglog1 libpq5 libbpf1 \
    linux-perf curl \
    && rm -rf /var/lib/apt/lists/*

# Install FlameGraph tools
RUN git clone --depth 1 https://github.com/brendangregg/FlameGraph /opt/FlameGraph

COPY --from=builder /src/drop/drop/build/drop_agent /opt/drop/drop_agent
COPY --from=builder /src/drop/drop/build/drop_server /opt/drop/drop_server
COPY --from=builder /src/drop/drop/common/bpf/offcpu.bpf.o /opt/drop/bpf/offcpu.bpf.o

# Ensure BPF filesystem is mounted
RUN mkdir -p /sys/fs/bpf /sys/kernel/debug

WORKDIR /opt/drop
