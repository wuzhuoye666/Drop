# Drop AI 协作规范

## 会话启动协议
1. **先读 memory-bank/activeContext.md** 恢复聚焦点
2. **再读 memory-bank/progress.md** 了解当前进度
3. **按需读取** architecture.md / requirements-traceability.md / implementation-plan.md
4. **锚定参考** DROP产品文档.md 作为需求的唯一权威来源

## 会话结束协议
1. **更新 activeContext.md**：填写本次做了什么、下一步做什么、当前阻塞
2. **更新 progress.md**：更新Phase状态、需求完成度、测试覆盖率、会话日志
3. **更新 requirements-traceability.md**：如有新完成的需求项，修改状态
4. **提交代码**：commit message必须解释"为什么"，不用update/fix/wip

## 上下文刷新策略
- 每次新会话自动从memory-bank恢复，不依赖历史对话
- 如果activeContext.md的"上次会话摘要"足够清晰，直接继续
- 如果不清楚，先读progress.md定位，再读对应源码确认

## 编码约束
- 状态迁移：必须封装为 `updateTaskStatus(tid, newStatus, reason)` 函数
- 心跳：Agent端1Hz心跳线程与工作线程必须分离（不能被采集阻塞）
- eBPF：容器配置 privileged + hostPID + 挂载 /sys/kernel/debug
- gRPC版本：C++侧锁定 grpc 1.48.x + protobuf 3.20.x
- 测试：单测≥50%，3个e2e测试（正常+不存在PID+超时）
- 日志：结构化JSON，apiserver用Zap，drop用glog，analysis用logging模块
- 错误处理：禁止空catch/except，必须有reason写库

## 需求争议仲裁
- 当实现与产品文档冲突时，以 DROP产品文档.md 为准
- 当memory-bank记录与代码现状冲突时，以代码为准，更新memory-bank

## 目录结构
- 项目结构详见 PROJECT_STRUCTURE.md
- memory-bank/ 为进度记忆专用，不放代码
