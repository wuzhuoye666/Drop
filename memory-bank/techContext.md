---
name: techContext
description: Drop项目技术栈、工具链、开发环境与依赖版本锁定
type: project
---

# Drop 技术上下文

## 语言与版本

| 组件 | 语言 | 最低版本 | 备注 |
|------|------|----------|------|
| drop | C++14 | GCC 9+ | CMake 3.16+ |
| apiserver | Go | 1.18+ | Gin v1.10, GORM v1.25 |
| analysis | Python | 3.8+ | psycopg2, pandas |
| web_frontend | Node.js | 16+ | React 18, D3 v7 |
| java_heap_analyzer | Go | 1.18+ | 子项目 |

## 核心依赖版本锁定

### C++ (drop)
- gRPC: 1.48.x
- Protobuf: 3.20.x
- glog / gflags
- nlohmann/json (配置解析)
- libbpf (eBPF采集器)
- CMake 3.16+

### Go (apiserver)
- Gin v1.10
- GORM v1.25 (PostgreSQL driver)
- grpc-go 1.48
- Viper (配置)
- Zap (日志)
- robfig/cron v3
- MinIO Go SDK

### Python (analysis)
- psycopg2-binary
- minio
- pandas
- matplotlib
- pako (前端解压，非Python侧)
- Perl (flamegraph.pl依赖)

### React (web_frontend)
- React 18
- react-router 6
- Zustand (状态管理)
- axios
- D3 v7 + d3-flame-graph
- TDesign 组件库
- ECharts (时序图)

### 基础设施
- PostgreSQL 14
- MinIO (替代COS)
- Docker 20.10+ / Docker Compose v2
- Linux Kernel ≥ 5.8 (eBPF CAP_BPF支持)

## 关键环境变量

```env
# apiserver
DROP_GRPC=drop_server:50051
PG_DSN=host=postgres user=postgres password=dev dbname=drop sslmode=disable
S3_ENDPOINT=minio:9000
S3_ACCESS_KEY=drop
S3_SECRET_KEY=dropdrop
S3_BUCKET=drop-data

# web_frontend
HOST_URL=http://localhost:8191
```

## perf_event_paranoid
必须设为1或更低：`echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid`
或给二进制 `setcap cap_perfmon,cap_sys_resource=ep`

## Docker Compose特异配置

eBPF Agent容器需求：
- `privileged: true` 或 `cap_add: [SYS_ADMIN, PERFMON]`
- `pid: host` (需要看到主机进程)
- 挂载 `/sys/kernel/debug` 和 `/sys/fs/bpf`

## Why: 版本不一致是C++ gRPC项目最常见的踩坑点；eBPF权限是演示环节的硬门槛
