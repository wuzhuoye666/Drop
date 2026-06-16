---
name: activeContext
description: 当前vibecoding会话的聚焦点、正在做的事、下一步行动
type: project
---

# 活跃上下文

> **每次新会话开始时先阅读此文件恢复上下文，每次会话结束前更新此文件**

## 当前聚焦

- **组件**：全局
- **Phase**：Phase 1 ✅ 已完成
- **正在做**：Phase 1 全部7个Step验收通过并提交
- **下一步**：开始 Phase 2 Step 2.1 — GORM Model 定义7张表 + AutoMigrate

## 当前阻塞

- 无

## 本次会话需注意

- Phase 1 全部验收通过: 目录骨架、5个proto、C++空壳编译、Go apiserver /healthz、React build、docker-compose postgres+minio healthy、Makefile proto/build/test/clean
- Go toolchain 自动升级到 1.25（gin v1.12 强制），grpc-go v1.81
- proto import 路径需用相对路径(common.proto)，不要用全路径(drop/common/proto/common.proto)
- 生成的 pb 文件输出到 drop/build/gen(C++) 和 apiserver/proto(Go)，已在 .gitignore

## 上次会话摘要

- 创建monorepo全部目录骨架(.gitkeep占位)
- 编写5个proto: common/healthcheck/hotmethod/control/init，字段与产品文档1.3节一致
- C++ CMake编译drop_server(4个gRPC service UNIMPLEMENTED)和drop_agent(gflags解析退出)
- Go apiserver: Gin+Viper+Zap, /healthz返回ok, JSON日志
- React(Vite+TS+TDesign+Zustand+Axios+D3), npm run build通过
- docker-compose: postgres14+minio, healthcheck验证healthy
- Makefile: proto/build/test/demo/clean全部目标可用
