# Makefile for eBPF kernel probes
# Compiles .bpf.c → .bpf.o using clang with BPF target

ARCH := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')
CLANG ?= clang-17
INCLUDES := -I/usr/include/$(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)

BPF_CFLAGS := -g -O2 -target bpf \
    -D__TARGET_ARCH_$(ARCH) \
    -Wall -Werror \
    $(INCLUDES) \
    -I. \
    -I/usr/include

BPF_SOURCES := $(wildcard *.bpf.c)
BPF_OBJECTS := $(patsubst %.bpf.c,%.bpf.o,$(BPF_SOURCES))

.PHONY: all clean

all: $(BPF_OBJECTS)

%.bpf.o: %.bpf.c vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@
	@echo "Compiled $@"

clean:
	rm -f *.bpf.o
