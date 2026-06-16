---
name: projectbrief
description: Drop项目核心定位、目标与约束条件
type: project
---

# Drop 项目简报

## 一句话定义
面向 Linux 服务器与容器场景的**按需性能采集 + 可视化分析**平台。

## 核心价值
用户在 Web UI 点击"采集"，平台将 perf / async-profiler / eBPF / pprof / memray 等工具下发到目标机器执行，收回原始数据，跑分析（火焰图、热点、内存泄漏、AI建议），最终可视化呈现。

## 四大组件

| 组件 | 语言 | 进程形态 | 职责 |
|------|------|----------|------|
| drop | C++14 | drop_agent + drop_server | 远程采集探针 + 调度中枢，gRPC通信 |
| apiserver | Go/Gin | HTTP服务 | REST对前端，gRPC对drop，PG持久化，MinIO存储 |
| analysis | Python | 一次性脚本/Job | perf.data → SVG火焰图 / TopN / AI建议 |
| web_frontend | React | 静态SPA | 登录→建任务→看火焰图 |

## 硬性约束（源自产品文档）

1. **状态机严格**：PENDING → RUNNING → UPLOADING → DONE / FAILED，每次迁移**必须落库并带 reason 字段**
2. **心跳机制**：Agent 每5s心跳，Server 30s无心跳判离线，离线/恢复**必须有审计日志**
3. **工程质量**：单测覆盖 ≥ 50%、≥ 3 个端到端集成测试（正常+2类异常）
4. **eBPF必须真跑**：现场触发异常，eBPF采集器必须能采到并在Web上看到分布变化
5. **一键部署**：`docker compose up` + `make demo` 10分钟内跑通
6. **提交规范**：commit message 必须"解释为什么"，拒绝 update/fix/wip
7. **多采集器**：perf之外必须有 eBPF采集器 + 用户态语言级采集器（py-spy/async-profiler/pprof三选一）
8. **Continuous Profiling**：常驻低频采样、定时切割，按时间轴回溯任意5分钟窗口火焰图

## Why: 这是对真实5万行+生产系统的复刻，评审标准是"能在一台干净Ubuntu上一键跑通并演示完整链路"
