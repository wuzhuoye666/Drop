---
name: architecture
description: Drop系统架构决策、数据流、状态机与组件间契约
type: project
---

# Drop 架构设计

## 系统拓扑

```
┌─────────────────────┐   REST/JSON   ┌──────────────────────┐
│  web_frontend (React)│ ───────────→  │  apiserver (Go/Gin)  │
│  火焰图/任务列表/AI建议│ ←───────────  │  鉴权/编排/PG/MinIO   │
└─────────────────────┘               └──────────┬───────────┘
                                                  │ gRPC (Control)
                                                  ▼
                                     ┌──────────────────────┐
                                     │  drop_server (C++)    │
                                     │  任务队列/心跳/状态机  │
                                     └──────────┬───────────┘
                                                  │ gRPC (HealthCheck + Hotmethod)
                                                  ▼
                                     ┌──────────────────────┐
                                     │  drop_agent (C++)     │
                                     │  perf/eBPF/async-proc │
                                     │  心跳1Hz/自监控PidStats│
                                     └──────────┬───────────┘
                                                  │ 产出文件
                                                  ▼
                                     ┌──────────────────────┐
                                     │  MinIO (对象存储)      │
                                     └──────────────────────┘
                                                  ▲
                                     ┌────────────┴─────────┐
                                     │ analysis (Python Job) │
                                     │ 火焰图/TopN/AI建议    │
                                     └──────────────────────┘
```

## 核心数据流（一次完整任务）

```
[1] 前端 POST /api/v1/tasks → apiserver
[2] apiserver 写 hotmethod_task(status=PENDING) → PG
[3] apiserver gRPC ControlService.CreateTask → drop_server
[4] drop_server 推入 tasks_[targetIP] 队列
[5] drop_agent 每5s HealthCheck.Do() 心跳 → 拉到 TaskDesc
[6] Agent: 更新status=RUNNING(reason:"开始采集") → fork perf/eBPF子进程采集N秒
[7] 采集完: 更新status=UPLOADING(reason:"上传结果") → 上传MinIO
[8] Agent: gRPC NotifyResult → Server → 写库status=DONE/FAILED(reason:...)
[9] apiserver检测到status=DONE → 触发 analysis Job
[10] analysis: 拉数据 → 生成火焰图/TopN/AI建议 → 写回MinIO + PG
[11] 前端轮询到分析完成 → 渲染结果
```

## 状态机

```
PENDING ──→ RUNNING ──→ UPLOADING ──→ DONE
  │            │            │
  │            ▼            ▼
  └──────→ FAILED ←────────┘
```

**规则**：
- 每次迁移必须 `UPDATE hotmethod_task SET status=?, status_info=? WHERE tid=?`
- reason/status_info 不能为空，最少写一句话说明
- FAILED 可以通过 retry 接口重新变为 PENDING

## gRPC 协议（4个服务）

1. **HealthCheck** (Agent → Server): 心跳 + 拉取任务
2. **Hotmethod** (Agent ↔ Server): 下发采集 + 上报结果
3. **Control** (apiserver → Server): CreateTask/FetchData/StatAgent
4. **Init** (Agent → Server): 注册 + 拉取COS配置

## 对象存储路径约定

```
/<tid>/perf.data          # 原始采集文件
/<tid>/flamegraph.svg     # 火焰图
/<tid>/top.json           # TopN热点
/<tid>/suggestions.md     # 规则建议
/<tid>/ai_suggestion.md   # AI建议
```

## 关键架构决策

### 决策1：状态机实现方式
**选择**：自研轻量状态机 + PG事务，不用Temporal
**Why**：Temporal引入额外基础设施（Server+持久化），对一键部署和评审复杂度影响大；PG事务+行锁足以保证状态迁移原子性和审计日志；C++端用mutex+condition_variable保护内存队列
**How to apply**：所有状态迁移封装为 `updateTaskStatus(tid, newStatus, reason)` 函数，内部用 `SELECT FOR UPDATE` + `UPDATE` 同一事务

### 决策2：eBPF采集器方案
**选择**：libbpf-go绑定，用户态Go程序加载预编译BPF对象
**Why**：C++ Agent中可用libbpf C API；bcc/bpftrace需要运行时编译且依赖LLVM，Docker镜像体积大；纯libbpf方案可预编译.bpf.o，镜像小且启动快
**How to apply**：eBPF探针预编译为.bpf.o，随Docker镜像分发；Agent启动时根据内核版本选择匹配的对象加载

### 决策3：Analysis调度方式
**选择**：apiserver通过 `docker exec` 或 `subprocess` 调起analysis容器
**Why**：K8s Job对单机docker compose场景过重；直接subprocess最简单可控
**How to apply**：apiserver定时轮询 status=DONE 且 analysis_status=0 的任务，调起 `docker exec analysis_runner python3 hotmethod_analyzer.py --task-id <tid>`

### 决策4：Memory Bank组织方式
**选择**：集中式单体bank在项目根目录，通过section区分组件
**Why**：vibecoding会话通常只聚焦一个组件，但需要跨组件上下文理解依赖关系；多余4个bank会导致切换成本高
**How to apply**：一个memory-bank/目录，用文件职责分离（进度、架构、需求映射等），activeContext.md标注当前聚焦组件
