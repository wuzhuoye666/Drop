---
name: activeContext
description: 当前vibecoding会话的聚焦点、正在做的事、下一步行动
type: project
---

# 活跃上下文

## 当前聚焦

- **组件**：全组件
- **Phase**：Phase 5 补验收 ✅ 大部分完成
- **正在做**：Docker 构建验证 (Step 9) — 构建中
- **下一步**：Phase 6 用户态语言级采集器

## 当前阻塞

- Docker 构建超时，等待中

## 本次会话摘要

### 发现的问题（审计）
1. **BPF 错误采集进程 Bug** — `bpf_get_current_task()` 返回 next 而非 prev → 改用 `ctx->prev_pid`
2. **悬空指针 Bug** — map 迭代 error path 中 `key_ptr` 指向局部变量 → 删除 error path 的 `key_ptr` 赋值
3. **addr2line fork 炸弹** — 每 frame 都 popen → 改为批量 `BatchResolveUserSymbols`
4. **map 不清理会耗尽** — poll 后不删除 entries → poll 结尾删除 counts 和 stack_ids
5. **缓存无驱逐** — g_user_maps_cache 无限增长 → 256 条 FIFO 驱逐
6. **demo 脚本 TID Bug** — `.data.task.tid` 应为 `.data.tid`
7. **测试全部缺失** — C++ 0 测试、Go 0 eBPF 测试、Python 0 测试

### 修复与新增
- 修复了以上全部 6 个代码 Bug
- 新增 C++ GTest 7 个测试用例（`ctest -R ebpf` PASS）
- 新增 Go 2 个 eBPF API 测试（PASS）
- 新增 Python 6 个 analysis 测试（PASS）
- 修复 demo_ebpf.sh TID 提取
- 完成了全链路 E2E 集成验证（创建→采集→分析→产物就绪，15秒内完成）
- 验证了 3 个产物文件（collapsed_ebpf.txt、flamegraph_offcpu.svg、top.json）
- 验证了 SVG 标题 "Off-CPU Flame Graph"
- 更新了 architecture.md 决策2（libbpf-go → libbpf C API）
