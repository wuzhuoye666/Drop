---
name: progress
description: Drop项目各Phase与需求的实时进度追踪
type: project
---

# Drop 进度追踪

> 每次vibecoding会话结束前必须更新此文件

## Phase 进度

| Phase | 名称 | 状态 | 当前Step | 最后更新 |
|-------|------|------|----------|----------|
| 1 | 脚手架与协议 | ✅ 已完成 | S1.1-S1.7 全部完成 | 2026-06-16 |
| 2 | PG模型+API骨架 | ✅ 已完成 | S2.1-S2.6 全部完成 | 2026-06-16 |
| 3 | Agent+Server心跳与任务下发 | ✅ 已完成 | S3.1-S3.7 全部完成 | 2026-06-16 |
| 4 | perf采集全链路 | ⬜ 未开始 | - | - |
| 5 | eBPF采集器 | ⬜ 未开始 | - | - |
| 6 | 用户态语言级采集器 | ⬜ 未开始 | - | - |
| 7 | Continuous Profiling | ⬜ 未开始 | - | - |
| 8 | 智能归因+NL (加分) | ⬜ 未开始 | - | - |
| 9 | 测试加固+部署 | ⬜ 未开始 | - | - |

## Phase 1 详细Step状态

| Step | 内容 | 状态 |
|------|------|------|
| 1.1 | monorepo目录结构+.gitignore+.gitkeep | ✅ |
| 1.2 | 5个.proto文件 | ✅ |
| 1.3 | C++ CMake编译空壳二进制(drop_server+drop_agent) | ✅ |
| 1.4 | Go apiserver空壳启动(/healthz) | ✅ |
| 1.5 | React(Vite+TS)项目初始化 | ✅ |
| 1.6 | docker-compose.yml(postgres+minio) | ✅ |
| 1.7 | Makefile全局目标 | ✅ |

## Phase 2 详细Step状态

| Step | 内容 | 状态 |
|------|------|------|
| 2.1 | GORM Model 7张表+2张审计表+AutoMigrate | ✅ |
| 2.2 | 12个核心API(gRPC先mock) | ✅ |
| 2.3 | 鉴权中间件(Cookie+dev-mode) | ✅ |
| 2.4 | MinIO Storage接口实现 | ✅ |
| 2.5 | 结构化日志+access log中间件 | ✅ |
| 2.6 | Go单测框架搭建 | ✅ |

## Phase 3 详细Step状态

| Step | 内容 | 状态 |
|------|------|------|
| 3.1 | HealthCheckService：心跳+拉取任务 | ✅ |
| 3.2 | ControlService：apiserver→Server 下发任务 | ✅ |
| 3.3 | Agent心跳线程+工作线程分离 | ✅ |
| 3.4 | Agent守护化(daemonize) | ✅ |
| 3.5 | Agent自监控PidStats | ✅ |
| 3.6 | Server 30s无心跳判离线+审计日志 | ✅ |
| 3.7 | 状态机完整实现与落库 | ✅ |

## 需求完成度汇总

| 类别 | 总数 | 已完成 | 完成率 |
|------|------|--------|--------|
| 基础能力 B1-B6 | 6 | 2 | 33% |
| 扩展能力 E1-E3 | 3 | 0 | 0% |
| 加分项 E4-E5 | 2 | 0 | 0% |
| 工程要求 G1-G4 | 4 | 0 | 0% |

## 测试覆盖率

| 组件 | 当前覆盖率 | 目标 |
|------|-----------|------|
| drop (C++) | 0% | ≥50% |
| apiserver (Go) | ~15% (22 tests covering statemachine, handler, middleware, storage) | ≥50% |
| analysis (Python) | 0% | ≥50% |
| web_frontend (JS/TS) | 0% | - |

## 端到端集成测试

| # | 场景 | 状态 |
|---|------|------|
| 1 | 正常路径：创建CPU任务→完成→火焰图 | ⬜ |
| 2 | 异常路径：不存在的PID→FAILED | ⬜ |
| 3 | 异常路径：采集超时→FAILED | ⬜ |

## 会话日志

| 日期 | 会话主题 | 完成事项 | 遗留问题 |
|------|---------|---------|---------|
| 2026-06-16 | 项目初始化 | 创建memory-bank、确定架构、细化实现计划 | - |
| 2026-06-16 | Phase 1 实施 | 全部7步验收通过并提交 | - |
| 2026-06-16 | Phase 2 实施 | 全部6步验收通过：GORM models、12个API、鉴权、MinIO存储、日志、单测 | - |
| 2026-06-16 | Phase 3 实施 | 全部7步验收通过：心跳+分发、守护化、PidStats、离线检测+审计、状态机落库 | - |
