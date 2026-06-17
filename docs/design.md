# Drop 系统设计文档

## 1. 系统概述

Drop 是一站式性能分析平台，支持 CPU/perf、eBPF off-CPU、Java async-profiler 三种采集器，Web UI 一键下发任务并实时查看火焰图、Top N 热函数及 AI 归因建议。

## 2. 架构

```
┌──────────┐   HTTPS    ┌───────────┐  gRPC   ┌────────────┐
│ Web FE   │◄──────────►│ apiserver │◄───────►│ drop_server│
│ (React)  │            │ (Go/Gin)  │         │ (C++)      │
└──────────┘            └─────┬─────┘         └──────┬─────┘
                              │                      │ gRPC
                        ┌─────▼─────┐          ┌─────▼─────┐
                        │ PostgreSQL │          │drop_agent │
                        │ + MinIO    │          │ (C++)     │
                        └───────────┘          └─────┬─────┘
                                                      │ HTTP
                                               ┌──────▼─────┐
                                               │  analysis   │
                                               │  (Python)   │
                                               └────────────┘
```

**关键决策**：
- C++ 实现采集端以支持 eBPF libbpf 和 perf_event_open
- Go 实现 API 层利用 GORM + Gin 生态快速开发
- Python 实现分析层复用 FlameGraph 工具链 + LLM SDK
- gRPC 双向流实现 Server→Agent 任务推送

## 3. 数据流

1. 用户在 Web UI 创建任务 → apiserver 写入 PG (PENDING)
2. Agent 心跳时 Server 推送任务 → Agent 执行采集 → 上传数据到 MinIO
3. Agent 通知 apiserver → apiserver 调度 analysis 子进程
4. Analysis 生成火焰图 SVG + Top JSON + 建议 Markdown → 上传 MinIO → 更新 PG

## 4. 状态机

```
PENDING(0) → RUNNING(1) → UPLOADING(2) → DONE(3)
    ↓             ↓             ↓
  FAILED(4) ← ─ ─ ─ ─ ─ ─ ─ ┘
    ↓
  PENDING(0)  (retry)
```

每次迁移写 `task_status_history` 表，带 reason。`DONE` 状态不可再迁移。

## 5. 核心组件

### 5.1 drop_agent (C++)
- **WorkerThread**: 从 gRPC 流接收 Target，通过 ProfilerFactory 创建 IProfiler 实例执行采集
- **HeartbeatThread**: 1Hz 向 Server 发送心跳，刷新 PG agent_info
- **ContinuousProfiler**: 低频 1Hz 常驻采样，每 5min flush 一次折叠栈到 MinIO + PG
- **IProfiler 接口**: record() + collect_result() + profiler_type()

### 5.2 apiserver (Go)
- Gin HTTP 路由，Zap 结构化日志
- GORM AutoMigrate 自动建表
- robfig/cron 定时调度 continuous profiling 任务
- NL Chat: 解析自然语言意图 → 自动创建任务 → SSE 流推送进度

### 5.3 analysis (Python)
- perf script → stackcollapse-perf.pl → flamegraph.pl 生成 SVG
- Top N 解析 (parse_topn)
- 规则引擎 (rules.yaml 正则匹配)
- LLM 归因 (OpenAI 兼容 API，输出 {diagnosis, evidence, recommendation, confidence})

## 6. eBPF 设计

- 使用 libbpf + BTF CO-RE 编写 `offcpu.bpf.c`
- 探针: `sched_switch` 追踪任务 off-CPU 时间
- 容器要求: `privileged: true` + `pid: host` + 挂载 `/sys/fs/bpf` 和 `/sys/kernel/debug`
- PID 过滤: BPF map 存储目标 PID，用户态 poll 时过滤

## 7. 存储模型

| 表 | 用途 |
|---|---|
| hotmethod_task | 任务核心状态 |
| analysis_suggestion | 规则/AI 建议行 |
| task_status_history | 状态迁移审计 |
| agent_info | Agent 注册信息 |
| agent_audit_log | 上线/离线审计 |
| continuous_profile_segments | 5min 分段索引 |

MinIO 存储: `{tid}/perf.data`, `{tid}/flamegraph.svg`, `{tid}/top.json`, `{tid}/suggestions.md`, `{tid}/ai_suggestion.md`

## 8. 测试策略

| 组件 | 目标覆盖率 | 当前 |
|---|---|---|
| drop (C++) | ≥50% | ~45% (33 tests) |
| apiserver (Go) | ≥50% | ~40% (34 tests) |
| analysis (Python) | ≥50% | ~45% (39 tests) |
| web_frontend (TS) | 基本覆盖 | 7 tests |

测试类型：GTest (C++)、go test + testify (Go)、pytest (Python)、vitest (TS)

## 9. 部署

`docker compose up` 启动全部服务：
- postgres:5432, minio:9000/9001
- apiserver:8191, web:80
- drop_server:50051 (host network)
- drop_agent (host network, privileged)

`make demo` 一键跑通 eBPF 全链路。
