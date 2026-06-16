---
name: activeContext
description: 当前vibecoding会话的聚焦点、正在做的事、下一步行动
type: project
---

# 活跃上下文

> **每次新会话开始时先阅读此文件恢复上下文，每次会话结束前更新此文件**

## 当前聚焦

- **组件**：全组件 (C++ agent/server + Go apiserver + Python analysis + React frontend)
- **Phase**：Phase 5 ✅ 已完成
- **正在做**：Phase 5 全部7个Step验收通过
- **下一步**：开始 Phase 6 Step 6.1 — 用户态语言级采集器

## 当前阻塞

- 无

## 本次会话需注意

- eBPF off-CPU探针：offcpu.bpf.c 使用 tracepoint/sched/sched_switch 捕获被切换出的进程栈
- EbpfLoader类：load→attach(pid)→poll(duration,output_path)→detach→unload
- 用户态符号解析：读/proc/<pid>/maps + addr2line回退
- EbpfProfiler：继承IProfiler，profiler_type=3，产出collapsed_ebpf.txt
- analysis: profiler_type=3 → analyze_ebpf() → flamegraph.pl --color=io --title="Off-CPU Flame Graph"
- 前端: profiler_type=3 时Tab标签 "Off-CPU Flame Graph", 找flamegraph_offcpu.svg
- Docker特权配置: privileged=true, pid=host, 挂载 /sys/fs/bpf, /sys/kernel/debug:ro, /lib/modules:ro
- demo脚本: scripts/demo_ebpf.sh 使用dd+stress-ng触发IO/调度压力

## 上次会话摘要

- 完成 Phase 5 eBPF采集器全部7个Step
- offcpu.bpf.c: tracepoint/sched/sched_switch + BPF_MAP_TYPE_STACK_TRACE
- EbpfLoader: libbpf C API加载.bpf.o，attach tracepoint，poll duration秒，遍历counts map，解析kallsyms+user maps+addr2line
- EbpfProfiler: IProfiler实现，profiler_type=3，工厂注册
- analysis: analyze_ebpf() → flamegraph_offcpu.svg (io色系，Off-CPU标题)
- 前端: 详情页根据profiler_type显示 "Off-CPU Flame Graph" Tab
- Docker: privileged+pid:host+BPF+debugfs挂载，注释说明每个权限用途
- demo脚本: dd IO压力 + stress-ng调度压力，对比基线和压力期火焰图
