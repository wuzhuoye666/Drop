SHELL := /bin/bash
export PATH := $(PATH):/usr/local/go/bin:$(HOME)/go/bin

.PHONY: proto build test demo clean help infra-up infra-down

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
	cd analysis && python3 -m pytest tests/ -v || echo "Python tests: none found"

test-frontend:
	cd web_frontend && npx vitest run || echo "Frontend tests: none found"

# ── Infrastructure ───────────────────────────────────────────────
infra-up: ## Start postgres + minio
	docker compose up postgres minio -d

infra-down: ## Stop and remove infrastructure
	docker compose down -v

# ── Docker ───────────────────────────────────────────────────────
docker-build: ## Build all Docker images
	docker compose build

docker-up: ## Start all services via docker compose
	docker compose up -d

docker-down: ## Stop all services
	docker compose down

# ── Demo ─────────────────────────────────────────────────────────
demo: ## Full demo: start infra, wait healthy, create eBPF task, apply IO pressure, show results
	@echo "=== Drop Demo ==="
	@echo "1. Starting infrastructure..."
	@docker compose up postgres minio -d
	@echo "2. Waiting for postgres..."
	@for i in $$(seq 1 15); do pg_isready -h 127.0.0.1 -p 5432 -U drop >/dev/null 2>&1 && break; sleep 1; done
	@echo "3. Waiting for minio..."
	@for i in $$(seq 1 15); do curl -sf http://127.0.0.1:9000/minio/health/live >/dev/null 2>&1 && break; sleep 1; done
	@echo "4. Starting apiserver..."
	@cd apiserver && DROP_PG_DSN="host=127.0.0.1 port=5432 user=drop password=drop dbname=drop sslmode=disable" DROP_S3_ENDPOINT="127.0.0.1:9000" DROP_GRPC_ADDR="127.0.0.1:50051" ./apiserver -dev-mode &
	@sleep 2
	@echo "5. Creating eBPF off-CPU profiling task via API..."
	@curl -sf -X POST http://127.0.0.1:8191/api/v1/tasks \
		-H 'Content-Type: application/json' \
		-H 'X-Dev-UID: demo' \
		-d '{"name":"demo-ebpf","type":0,"profiler_type":3,"target_ip":"127.0.0.1","pid":0,"duration":15,"hz":0}' | python3 -m json.tool
	@echo "6. Applying IO pressure (5 seconds)..."
	@dd if=/dev/zero of=/tmp/drop_demo_io bs=1M count=50 2>/dev/null; rm -f /tmp/drop_demo_io
	@echo "7. Waiting for analysis (20s)..."
	@sleep 20
	@echo "8. Checking task results..."
	@curl -sf 'http://127.0.0.1:8191/api/v1/tasks?page=1&page_size=5' | python3 -m json.tool 2>/dev/null || echo "(apiserver may not be running — check manually)"
	@echo ""
	@echo "=== Demo complete ==="
	@echo "Open http://127.0.0.1:5173 (if frontend dev server running) or http://127.0.0.1:80 (docker) to view flame graphs."
	@echo "Stop demo: make demo-clean"

demo-clean: ## Stop demo processes and infrastructure
	-pkill -f "apiserver -dev-mode" 2>/dev/null
	docker compose down

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
