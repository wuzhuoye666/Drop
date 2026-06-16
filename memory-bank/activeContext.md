---
name: activeContext
description: 当前vibecoding会话的聚焦点、正在做的事、下一步行动
type: project
---

# 活跃上下文

> **每次新会话开始时先阅读此文件恢复上下文，每次会话结束前更新此文件**

## 当前聚焦

- **组件**：全局（项目初始化阶段）
- **Phase**：Phase 1 准备中
- **正在做**：搭建memory-bank、确认项目结构与实现计划
- **下一步**：开始执行 Phase 1 Step 1.1 — 创建monorepo目录结构

## 当前阻塞

- 无

## 本次会话需注意

- 产品文档已读取并锚定在 `DROP产品文档.md`
- 架构决策已记录（自研状态机、libbpf、subprocess调度、集中式memory-bank）
- eBPF容器需要 privileged + hostPID + 挂载 /sys/kernel/debug
- 实现计划已细化到每个Step，每步有 `[ ]` 完成度标记和详细验收点
- Phase编号从1开始（不含Phase 0），与implementation-plan.md一致

## 上次会话摘要

- (首次正式会话) 创建了memory-bank全部文档、PROJECT_STRUCTURE.md、CODEBUDDY.md
- 将实现计划细化为9个Phase × 若干Step，每步含 `[ ]` 标记和3-6条验收命令
