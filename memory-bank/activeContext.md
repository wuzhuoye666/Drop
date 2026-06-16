---
name: activeContext
description: 当前vibecoding会话的聚焦点、正在做的事、下一步行动
type: project
---

# 活跃上下文

> **每次新会话开始时先阅读此文件恢复上下文，每次会话结束前更新此文件**

## 当前聚焦

- **组件**：drop_server + drop_agent (C++) + apiserver (Go)
- **Phase**：Phase 3 ✅ 已完成
- **正在做**：已完成 Phase 3 全部7步验收
- **下一步**：开始 Phase 4 Step 4.1 — PerfProfiler采集器实现

## 当前阻塞

- 无

## 本次会话需注意

- C++ gflags `DEFINE_string(version, ...)` 与 gflags 内置 `--version` 冲突，改名 `DEFINE_string(agent_version, ...)`
- `google::InitGoogleLogging` 必须在 `Daemonize()` 之后调用，否则 fd 0/1/2 被关闭后 glog 无法写入文件
- daemonize 前设置 `FLAGS_log_dir="/tmp"` 和 `FLAGS_logbufsecs=0` 确保日志立即写入文件
- PG `ON CONFLICT (ip_addr)` 需要唯一索引，GORM soft delete 的部分索引不满足要求
- agent_info 表的 `last_heartbeat_at` 列实际名为 `last_hb`（GORM 字段名映射）
- NotifyResult 直接设 DONE 会跳过中间状态导致状态机拒绝；必须按 RUNNING→UPLOADING→DONE 顺序迁移
- apiserver 启动后如果 drop_server 未就绪，gRPC 连接会失败标记任务 FAILED；需要确保服务启动顺序

## 上次会话摘要

- 完成 Phase 3 全部7个Step
- drop_server: 4个gRPC服务完整实现（HealthCheck.Do 心跳+分发任务、Hotmethod.NotifyResult 状态落库、Control.CreateTask 任务入队、StatAgent 查询、Init.RegisterAgent 注册）
- drop_agent: 心跳线程 1Hz + 工作线程分离、守护化(daemonize)、自监控 PidStats(cpu%/rss_mb/read_kbs/write_kbs)、信号处理优雅退出
- apiserver: dispatchTask 真正调用 gRPC ControlService.CreateTask，RetryTask 重新分发
- PG集成：agent_info upsert、task_status 事务迁移（SELECT FOR UPDATE）、agent_audit_log 审计
- 状态机验证：PENDING→RUNNING→UPLOADING→DONE 完整链路，task_status_history 每次4条审计记录
- 离线检测：30s无心跳标记 offline + 审计日志，上线恢复也写审计
- C++ CMake 从 C++14 升级至 C++17，新增 pkg-config 查找 libpq