---
name: activeContext
description: 当前vibecoding会话的聚焦点、正在做的事、下一步行动
type: project
---

# 活跃上下文

> **每次新会话开始时先阅读此文件恢复上下文，每次会话结束前更新此文件**

## 当前聚焦

- **组件**：全组件 (C++ agent/server + Go apiserver + Python analysis + React frontend)
- **Phase**：Phase 4 ✅ 已完成
- **正在做**：Phase 4 全部9步验收通过
- **下一步**：开始 Phase 5 Step 5.1 — eBPF内核态探针

## 当前阻塞

- 无

## 本次会话需注意

- PerfProfiler fallback: `-p <pid>` 失败时自动回退到 system-wide perf（容器 PID namespace 限制）
- PerfProfiler record() 使用 fork+execvp 禁止 system() 调用
- 超时机制：setpgid → SIGTERM → 5s → SIGKILL → waitpid 回收
- apiserver upload endpoint `/api/v1/tasks/:tid/upload/*filename` 不需要 cookie auth
- analysis hotmethod_analyzer.py 完整链路: PG→MinIO下载→perf script→stackcollapse→flamegraph.pl→topN→上传MinIO
- apiserver 分析调度器每10s扫描 status=3 DONE 且 analysis_status=0 的任务，spawn python 子进程
- agent upload 通过 popen+curl multipart/form-data 上传到 apiserver（MVP 方案，生产可换 libcurl）
- PG hotmethod_task 新增 cos_key 字段 (size 512)
- 前端 vite proxy /api → apiserver:8191

## 上次会话摘要

- 完成 Phase 4 全部9个Step
- PerfProfiler: IProfiler接口 + PerfProfiler实现 fork+execvp perf record + 超时杀进程机制 + PID namespace fallback
- Agent上传: 采集后 curl multipart POST 到 apiserver upload endpoint → MinIO 存储
- analysis: hotmethod_analyzer.py 完整链路 (PG参数+MinIO下载→perf script→stackcollapse→flamegraph→topJSON→上传)
- apiserver: upload endpoint + analysis scheduler (10s轮询)
- 前端: 首页Agent列表 + 任务列表+新建采样弹窗+状态轮询(3s) + 任务详情(火焰图iframe+TopN表格+AI Tab)
