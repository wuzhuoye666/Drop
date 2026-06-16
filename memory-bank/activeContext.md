---
name: activeContext
description: 当前vibecoding会话的聚焦点、正在做的事、下一步行动
type: project
---

# 活跃上下文

> **每次新会话开始时先阅读此文件恢复上下文，每次会话结束前更新此文件**

## 当前聚焦

- **组件**：apiserver (Go)
- **Phase**：Phase 2 ✅ 已完成
- **正在做**：已完成 Phase 2 全部6步验收
- **下一步**：开始 Phase 3 Step 3.1 — HealthCheck Service 心跳+拉取任务

## 当前阻塞

- 无

## 本次会话需注意

- GORM `Updates` 会修改传入的结构体，`oldStatus` 需在 `Updates` 之前保存
- Go v1.21 安装在 /usr/local/go/bin，需要 `export PATH=$PATH:/usr/local/go/bin`
- `go build -o apiserver .` 会与 proto 目录名冲突，改用 `go build -o bin/apiserver .`
- dev-mode 通过 `-dev-mode` flag 或 `server.dev_mode: true` 在 YAML 中设置
- grpcCC 非nil（NewClient是lazy的），所以dispatchTask goroutine会触发mock逻辑

## 上次会话摘要

- 完成 Phase 2 全部6个Step
- GORM Model: 7张业务表 + 2张审计表（task_status_history, agent_audit_history），AutoMigrate成功
- 12个API全部可实现：auth/check, users, agents, agent/stat, tasks(CRUD+retry), cosfiles, group(CRUD+member+agent), schedule/task
- updateTaskStatus 完整实现了 SELECT FOR UPDATE → 校验迁移 → UPDATE → INSERT history → 同事务commit
- 修复关键bug：Updates后task.Status被修改，需提前保存oldStatus
- MinIO Storage接口实现：Put/Get/PreSign/Delete/IsExist/ListObjects，bucket自动创建
- 鉴权中间件：Cookie取drop_user_uid/drop_user_name，无cookie→401，dev-mode自动注入
- 结构化access log：记录method/path/status/latency_ms，>10KB body截断
- Go单测：statemachine(14组迁移测试) + handler(4组集成测试) + middleware(3组) + storage(1组) 全部PASS
