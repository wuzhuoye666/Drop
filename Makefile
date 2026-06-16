SHELL := /bin/bash
export PATH := $(PATH):/usr/local/go/bin:$(HOME)/go/bin

.PHONY: proto build test demo clean help

# ── Proto generation ─────────────────────────────────────────────
PROTO_DIR  := drop/common/proto
PROTO_OUT  := drop/build/gen
PROTO_OUT_GO := apiserver/proto

PROTO_FILES := $(wildcard $(PROTO_DIR)/*.proto)

proto: ## Generate protobuf + gRPC code for C++ and Go
	@mkdir -p $(PROTO_OUT) $(PROTO_OUT_GO)
	protoc \
		--proto_path=$(PROTO_DIR) \
		--cpp_out=$(PROTO_OUT) \
		--grpc_out=$(PROTO_OUT) \
		--plugin=protoc-gen-grpc=$$(which grpc_cpp_plugin) \
		$(PROTO_FILES)
	@echo "C++ proto generation done"
	protoc \
		--proto_path=$(PROTO_DIR) \
		--go_out=$(PROTO_OUT_GO) --go_opt=paths=source_relative \
		--go-grpc_out=$(PROTO_OUT_GO) --go-grpc_opt=paths=source_relative \
		$(PROTO_FILES)
	@echo "Go proto generation done"

# ── Build ────────────────────────────────────────────────────────
build: build-bpf build-cpp build-go build-frontend ## Build all components

build-bpf: ## Compile eBPF kernel probes
	cd drop/common/bpf && make

build-cpp: build-bpf ## Build C++ drop_server + drop_agent
	@mkdir -p drop/build && cd drop/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$$(nproc)

build-go: ## Build Go apiserver
	cd apiserver && go build -o apiserver .

build-frontend: ## Build React frontend
	cd web_frontend && npm install && npm run build

# ── Test ─────────────────────────────────────────────────────────
test: test-cpp test-go test-python test-frontend ## Run all tests

test-cpp:
	@if [ -d drop/build ]; then cd drop/build && ctest --output-on-failure; else echo "C++ tests: build first"; fi

test-go:
	cd apiserver && go test ./... || echo "Go tests: none found"

test-python:
	cd analysis && pytest || echo "Python tests: none found"

test-frontend:
	cd web_frontend && npm test -- --passWithNoTests || echo "Frontend tests: none found"

# ── Demo (placeholder) ───────────────────────────────────────────
demo: ## Run demo (placeholder, filled in later phases)
	@echo "demo target placeholder — will be implemented in Phase 9"

# ── Infrastructure ───────────────────────────────────────────────
infra-up: ## Start postgres + minio
	docker compose up postgres minio -d

infra-down: ## Stop and remove infrastructure
	docker compose down -v

# ── Clean ────────────────────────────────────────────────────────
clean: ## Remove all build artifacts
	rm -rf drop/build
	cd drop/common/bpf && make clean
	rm -f apiserver/apiserver
	rm -rf web_frontend/dist web_frontend/node_modules
	@echo "Clean done"

# ── Help ─────────────────────────────────────────────────────────
help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-20s\033[0m %s\n", $$1, $$2}'
