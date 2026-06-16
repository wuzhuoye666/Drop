---
name: requirements-traceability
description: 产品文档需求到实现的追踪矩阵，确保不遗漏
type: project
---

# 需求可追溯性矩阵

## 基础能力

| # | 需求 | 来源(文档章节) | 实现位置 | 状态 | 完成度 |
|---|------|---------------|----------|------|--------|
| B1 | Web UI指定目标PID/采样时长/采样率，通过Server下发任务给Agent | 基础能力1 | web_frontend/pages/taskList (新建弹窗), apiserver/server/handler.go (CreateTask+dispatchTask), drop/agent/main.cpp (WorkerThread) | ✅ 已完成 | 100% |
| B2 | Agent采集存储目标进程性能数据(CPU/内存)，通知Analyzer | 基础能力2 | drop/common/src/perf_profiler.cpp (PerfProfiler::record), drop/agent/main.cpp (Upload+NotifyResult), analysis/drop_analyzer/hotmethod_analyzer.py | ✅ 已完成 | 100% |
| B3 | Analyzer转成Web可展现格式(火焰图D3/ECharts/热力图) | 基础能力3 | analysis/drop_analyzer/hotmethod_analyzer.py (perf script→stackcollapse→flamegraph.pl), web_frontend/pages/taskResult (iframe SVG) | ✅ 已完成 | 100% |
| B4 | 状态机PENDING→RUNNING→UPLOADING→DONE/FAILED，每次迁移落库带reason | 基础能力4 | apiserver/server/server.go(updateTaskStatus), drop/common/src/task_queue.cpp(PGStore::UpdateTaskStatus) | ✅ 已完成 | 100% |
| B5 | Agent每5s心跳，Server 30s无心跳判离线，Web有Agent列表，离线/恢复有审计日志 | 基础能力5 | drop/agent/main.cpp(HeartbeatThread), drop/server/main.cpp(HealthCheckService+HeartbeatScanner), apiserver/server/handler.go(StatAgent) | ✅ 已完成 | 100% |
| B6 | 结构化日志、显式错误处理、单测≥50%、≥3端到端集成测试 | 基础能力6 | 全组件，apiserver/middleware/log.go(Zap), drop使用glog，analysis使用logging模块 | 🔵 进行中 | 30% |

## 扩展能力

| # | 需求 | 来源(文档章节) | 实现位置 | 状态 | 完成度 |
|---|------|---------------|----------|------|--------|
| E1 | Continuous Profiling：常驻低频采样、定时切割、按时间轴回溯任意5分钟窗口 | 扩展-Continuous | drop/common/src/continuous_profiler.cpp (常驻低频采样+5min分段flush), apiserver/server/handler.go (CreateSegment+ListSegments+GetProfileWindow), apiserver/model/model.go (ContinuousProfileSegment表), apiserver/server/scheduler.go (robfig/cron调度), web_frontend/pages/taskResult (Timeline组件+区间选择+合并火焰图) | ✅ 已完成 | 100% |
| E2 | eBPF采集器：使用libbpf/bcc/bpftrace，至少一个内核态探针 | 扩展-多采集器1 | drop/common/bpf/offcpu.bpf.c (内核探针), drop/common/src/ebpf_loader.cpp (libbpf加载器), drop/common/src/ebpf_profiler.cpp (IProfiler实现) | ✅ 已完成 | 100% |
| E3 | 用户态语言级采集器(py-spy/async-profiler/pprof三选一)，有自己的可视化形态 | 扩展-多采集器2 | drop/common/src/async_profiler_profiler.cpp (IProfiler实现), drop/tools/install_async_profiler.sh, analysis/drop_analyzer/hotmethod_analyzer.py (analyze_async_profiler), web_frontend/pages/taskResult (Java Flame Graph Tab), web_frontend/pages/taskList (动态event参数) | ✅ 已完成 | 100% |
| E4 | 智能归因：火焰图+元数据+baseline结构化喂给LLM | 加分-智能归因 | analysis/analysis_advisor.py, analysis/hunyuanApi.py | ⬜ 未开始 | 0% |
| E5 | 自然语言采集：一句话描述意图→自动识别目标/选工具/定采样率/采完总结/追问 | 加分-NL采集 | apiserver/nl_handler.go, analysis/nl_parser.py | ⬜ 未开始 | 0% |

## 工程要求

| # | 需求 | 来源(文档章节) | 实现位置 | 状态 | 完成度 |
|---|------|---------------|----------|------|--------|
| G1 | docker compose up + make demo 一键跑通，10分钟内 | 交付物2 | docker-compose.yml, Makefile | ⬜ 未开始 | 0% |
| G2 | 提交历史完整，commit message解释"为什么" | 工程要求2 | 全仓库 | ⬜ 未开始 | 0% |
| G3 | eBPF必须真跑，现场触发异常能看到分布变化 | 工程要求3 | drop/agent/EbpfProfiler, demo脚本 | ⬜ 未开始 | 0% |
| G4 | ≤10页设计文档 | 交付物3 | docs/design.md | ⬜ 未开始 | 0% |

## 状态图例
- ⬜ 未开始 (0%)
- 🔵 进行中 (1-99%)
- ✅ 已完成 (100%)
