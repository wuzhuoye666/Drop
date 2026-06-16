---
name: implementation-plan
description: Drop项目Vibecoding实现顺序、步骤完成度与详细验收标准
type: project
---

# Drop Vibecoding 实现计划

## 核心原则

1. **先契约后实现**：先写.proto → 先写接口定义 → 再写业务逻辑
2. **先纵向后横向**：先打通一条完整链路(CPU perf采集→火焰图)，再扩展其他采集器
3. **先本地后分布式**：单个组件先可独立运行测试，再做docker compose联调
4. **每个Step产出可验证**：每步有独立验收点，不通过不前进

## 完成度图例

| 标记 | 含义 |
|------|------|
| `[ ]` | 未开始 |
| `[~]` | 进行中 |
| `[x]` | 已完成 |
| `[!]` | 阻塞/需修复 |

## Phase间质量门禁

每个Phase完成后，必须通过以下检查才能进入下一个Phase：

- [ ] 所有验收命令均执行通过（exit code 0）
- [ ] 无编译warning：`make build 2>&1 | grep -ci warning` 结果为 0
- [ ] 已有单测全过：`make test` 无失败
- [ ] PG数据一致：状态字段与实际逻辑一致，无脏数据
- [ ] memory-bank已更新：progress.md和requirements-traceability.md已同步

---

## Phase 1：脚手架与协议 `[ ]`

**目标**：空壳项目全部编译通过，4个.proto定义完毕，数据库和MinIO可启动

### Step 1.1 `[ ]` 创建monorepo目录结构
- 按 `PROJECT_STRUCTURE.md` 创建全部目录骨架
- 每个空目录放 `.gitkeep`
- 初始化 `git init`，创建 `.gitignore`（忽略build/、node_modules/、__pycache__/、*.pyc、.env）

**验收**：
1. `ls -d drop/ apiserver/ analysis/ web_frontend/ memory-bank/ docs/` 全部存在
2. `find . -empty -type d` 无输出（所有空目录有.gitkeep）
3. `git ls-files | wc -l` ≥ 5（至少有.gitignore和几个.gitkeep被追踪）

### Step 1.2 `[ ]` 编写5个.proto文件
- `common.proto`: PidStats / File / CosConfig / RecordArgv 消息
- `healthcheck.proto`: HealthCheck.Do RPC，HealthCheckRequest/Response
- `hotmethod.proto`: TaskDesc / TaskResult / Hotmethod.Collect + NotifyResult RPC
- `control.proto`: Control.CreateTask / FetchData / StatAgent RPC
- `init.proto`: InitAgentInfo.RegisterAgent / FetchConfig RPC
- 字段命名与 `DROP产品文档.md` 1.3节完全一致

**验收**：
1. `make proto` 退出码为 0
2. `ls drop/common/proto/*.pb.h drop/common/proto/*.grpc.pb.h` 文件存在
3. `ls apiserver/proto/*.pb.go apiserver/proto/*_grpc.pb.go` 文件存在
4. 人工抽样：`grep "taskID" drop/common/proto/hotmethod.pb.h` 命中（关键字段名与文档一致）

### Step 1.3 `[ ]` C++ CMakeLists.txt 编译空壳二进制
- `drop_server main.cpp`: 注册4个gRPC service（空实现返回 UNIMPLEMENTED）
- `drop_agent main.cpp`: 解析 gflags → 打印配置 → 退出
- CMake: `find_package(gRPC REQUIRED)`, `find_package(Protobuf REQUIRED)`
- 锁定 grpc 1.48.x + protobuf 3.20.x

**验收**：
1. `cd drop && mkdir -p build && cd build && cmake .. && make -j$(nproc)` 退出码 0
2. `./drop_server -port 50051 &` 启动不崩溃，进程PID存在
3. `./drop_agent -config /dev/null` 打印参数后退出码 0
4. `grpcurl -plaintext localhost:50051 list` 能列出4个service名

### Step 1.4 `[ ]` Go apiserver 空壳启动
- `go mod init` + require Gin/Viper/Zap/grpc-go
- `main.go`: Viper加载配置 → Gin router → `/healthz` 返回 `{"status":"ok"}`
- Zap logger初始化

**验收**：
1. `cd apiserver && go build -o apiserver .` 退出码 0
2. `./apiserver -c apiserver.yaml.example &`
3. `curl -sf http://localhost:8191/healthz | jq .status` 输出 `"ok"`
4. 日志输出为JSON格式（包含 level/msg/time 字段）

### Step 1.5 `[ ]` React项目初始化
- 用 Vite + React + TypeScript 初始化（或 CRA）
- 安装依赖：react-router, zustand, axios, d3, tdesign-react
- `npm run build` 成功

**验收**：
1. `cd web_frontend && npm install && npm run build` 退出码 0
2. `ls dist/index.html` 存在（Vite）或 `ls build/index.html`（CRA）
3. 用浏览器打开 `npm run dev` 能看到默认页面

### Step 1.6 `[ ]` docker-compose.yml 骨架
- postgres:14 服务 + healthcheck (`pg_isready`)
- minio 服务 + healthcheck + console(:9001)
- 配好 volume 持久化 + 网络

**验收**：
1. `docker compose up postgres minio -d` 退出码 0
2. `sleep 10 && docker compose ps` 两个服务均为 healthy
3. `docker compose exec postgres pg_isready` 输出 "accepting connections"
4. 浏览器访问 `http://localhost:9001` 能看到 MinIO Console 登录页
5. `docker compose down -v` 清理干净

### Step 1.7 `[ ]` Makefile 全局目标
- `make proto` → 编译所有.proto
- `make build` → 编译4个组件
- `make test` → 运行所有单测
- `make demo` → 占位（后续Phase填充）
- `make clean` → 清理构建产物

**验收**：
1. `make proto && make build` 连续执行退出码 0
2. `make test` 无测试时正常退出（输出 "no tests" 不报错）
3. `make clean && ls drop/build` 目录不存在或为空

---

## Phase 2：PG模型 + API骨架 `[ ]`

**目标**：apiserver 能读写PG，12个API可用（gRPC侧先mock），MinIO可读写

### Step 2.1 `[ ]` GORM Model 定义7张表 + AutoMigrate
- user_info, agent_info, hotmethod_task, multi_tasks, groups, group_members, analysis_suggestion
- 新增 task_status_history(tid, old_status, new_status, reason, timestamp) 审计表
- agent_audit_log(id, agent_id, event_type, reason, timestamp) 审计表
- hotmethod_task 必须有 status_info 字段存 reason
- 索引：tid UNIQUE, (target_ip, status) 复合索引

**验收**：
1. `docker compose up postgres -d`
2. `cd apiserver && go run main.go -c apiserver.yaml.example`（AutoMigrate执行）
3. `docker compose exec postgres psql -U postgres -d drop -c "\dt"` 输出包含9张表（7+2审计表）
4. `psql -c "\d hotmethod_task"` 确认 status(integer), status_info(text) 字段存在
5. `psql -c "\d task_status_history"` 确认 old_status, new_status, reason 字段存在

### Step 2.2 `[ ]` 实现12个核心API（gRPC先mock）
- 完整列表见 `architecture.md` 2.4节
- 创建任务时：写PG(status=0 PENDING, status_info="任务已创建") → mock gRPC调用（日志打印+返回成功）
- 所有状态迁移必须调用封装函数 `updateTaskStatus(tid, newStatus, reason)`
- `updateTaskStatus` 内部逻辑：`SELECT FOR UPDATE` → 校验合法迁移 → `UPDATE` → `INSERT task_status_history` → 同一事务commit

**验收**：
1. `curl -s -X POST http://localhost:8191/api/v1/tasks -H 'Content-Type: application/json' -b 'drop_user_uid=test;drop_user_name=tester' -d '{"name":"test","type":0,"profiler_type":0,"target_ip":"127.0.0.1","pid":1,"duration":10,"hz":99}' | jq .code` → 0
2. `curl -s http://localhost:8191/api/v1/tasks -b 'drop_user_uid=test;drop_user_name=tester' | jq '.data.list | length'` → ≥1
3. `psql -c "SELECT status, status_info FROM hotmethod_task LIMIT 1"` → status_info 非空
4. `psql -c "SELECT COUNT(*) FROM task_status_history"` → ≥1（创建时写了一条PENDING记录）

### Step 2.3 `[ ]` 鉴权中间件
- Cookie取 drop_user_uid / drop_user_name
- 无cookie → 返回 401 `{code: 4010001, data: {location: "..."}}`
- 开发模式flag `-dev-mode` 可跳过鉴权（自动注入test用户）

**验收**：
1. 无cookie请求 `curl -s -o /dev/null -w "%{http_code}" http://localhost:8191/api/v1/tasks` → 401
2. 带cookie请求 `curl -s -b "drop_user_uid=test;drop_user_name=tester" http://localhost:8191/api/v1/tasks | jq .code` → 0
3. dev模式启动 `./apiserver -c config.yaml -dev-mode` → 无cookie请求也返回200

### Step 2.4 `[ ]` MinIO Storage接口实现
- interface Storage: Put(key, reader) / Get(key) → io.ReadCloser / PreSign(key, expire) → url / Delete(key) / IsExist(key) → bool
- MinIO实现类，启动时自动 CreateBucketIfNotExists("drop-data")

**验收**：
1. 手动测试上传：通过临时测试端点或Go测试函数上传一个文件
2. `docker compose exec minio mc ls local/drop-data/` 能看到文件
3. PreSign返回的URL在浏览器中可直接下载文件
4. PreSign URL过期时间 ≥ 1小时

### Step 2.5 `[ ]` 结构化日志 + access log中间件
- Zap JSON logger：全局初始化，所有组件使用
- access log中间件：记录 method, path, status, latency_ms, body（>10KB截断）

**验收**：
1. 发起请求后，检查apiserver标准输出
2. access log为JSON格式，包含 `method`, `path`, `status`, `latency_ms` 字段
3. 上传 >10KB body时，日志中body被截断且有标记

### Step 2.6 `[ ]` 各组件单测框架搭建
- C++: Google Test（CMake FetchContent）
- Go: `testing` + `testify/assert`
- Python: `pytest` + `pytest-cov`
- Frontend: `jest` + `@testing-library/react`

**验收**：
1. `cd drop/build && cmake .. -DBUILD_TESTS=ON && make && ctest` 退出码 0
2. `cd apiserver && go test ./...` 退出码 0
3. `cd analysis && pytest` 退出码 0
4. `cd web_frontend && npm test` 退出码 0

---

## Phase 3：Agent + Server 心跳与任务下发 `[ ]`

**目标**：Agent真心跳，Server真能下发任务，状态机真实运转落库

### Step 3.1 `[ ]` HealthCheckService：心跳 + 拉取任务
- Agent: 1Hz循环调用 HealthCheck.Do()，发送 hostName/ipAddr/uid/PidStats
- Server: 收到心跳 → 更新 agent_info 表的 last_heartbeat_at → 检查 tasks_[ipAddr] 队列 → 有则 response.pending=true + 填入 taskDesc
- 要点：memory中的任务队列(tasks_)和PG中的任务状态要同步——CreateTask时写PG+推队列，心跳派发时更新PG status=1(RUNNING)

**验收**：
1. 启动 drop_server + drop_agent
2. Agent日志每秒出现心跳请求行
3. Server日志显示心跳响应（pending=false）
4. `psql -c "SELECT last_heartbeat_at FROM agent_info WHERE ip_addr='127.0.0.1'"` → 时间戳在5秒内

### Step 3.2 `[ ]` ControlService：apiserver → Server 下发任务
- CreateTask: 接收 targetIP + TaskDesc → `tasks_[targetIP].push_back(taskDesc)` (mutex保护) → 写PG status=0 PENDING
- StatAgent: 返回所有已注册Agent列表 + online状态
- 要点：Server启动时起一个后台线程同步PG中 agent_info 的 online 状态（替代纯内存map）

**验收**：
1. `grpcurl -plaintext -import-path drop/common/proto -proto control.proto -d '{"targetIP":"127.0.0.1","service":"hotmethod","taskDesc":{"taskID":"t001","taskType":0,"profilerType":0,"sampleArgv":{"hz":99,"duration":10,"pid":1234}}}' localhost:50051 Control/CreateTask`
2. Server日志显示 "task t001 pushed to queue for 127.0.0.1"
3. `psql -c "SELECT status, status_info FROM hotmethod_task WHERE tid='t001'"` → status=0, status_info非空

### Step 3.3 `[ ]` Agent心跳线程 + 工作线程分离
- 心跳线程：独立的 `std::thread`，1Hz循环，绝不阻塞
- 工作线程：从 `std::deque<TaskDesc>` 取任务执行（当前Phase只sleep对应duration模拟采集）
- 通信：`std::mutex` + `std::condition_variable`
- 心速线程读到pending=true时，将taskDesc投递到工作队列

**验收**：
1. 下发一个duration=10秒的任务
2. Agent日志：心跳线程持续打印（每秒1行，无间断）
3. Agent日志：工作线程显示 "task t001 started" 和 "task t001 finished"
4. 心跳无间断（即使工作线程在忙）

### Step 3.4 `[ ]` Agent守护化(daemonize)
- 实现：fork→父退出→setsid→再fork→关0/1/2→重定向/dev/null
- 写PID文件 `/tmp/drop_agent.pid`
- 支持 `-foreground` flag 跳过守护化

**验收**：
1. 不带flag启动：`./drop_agent -config config.json` → 进程后台化
2. `cat /tmp/drop_agent.pid` → PID数字，`ps -p <PID>` 存在
3. 带flag启动：`./drop_agent -config config.json -foreground` → 日志打到终端

### Step 3.5 `[ ]` Agent自监控PidStats
- 每1秒读 `/proc/self/stat` → CPU% = (utime+stime差值)/(间隔×clk_tck)×100
- 读 `/proc/self/statm` → RSS = 第2字段×pagesize
- 读 `/proc/self/io` → read_bytes/write_bytes差值 → KB/s
- 填入 HealthCheckRequest.selfPstats / childrenPstats（遍历/proc/self/task/下子进程）

**验收**：
1. Agent日志中出现 PidStats 数据行（cpu_percent, rss_mb, read_kbs等）
2. cpu_percent > 0（Agent本身占用CPU）
3. rss_mb 数值合理（10~100MB量级）

### Step 3.6 `[ ]` Server 30s无心跳判离线 + 审计日志
- Server后台线程每10秒扫描 agent_info 表
- 当前时间 - last_heartbeat_at > 30秒 且 online=true → UPDATE online=false, INSERT agent_audit_log(event_type='offline', reason='30s无心跳')
- 检测到 online=false → true → INSERT agent_audit_log(event_type='online', reason='心跳恢复')

**验收**：
1. Agent正常运行时 `psql "SELECT online FROM agent_info WHERE ip_addr='127.0.0.1'"` → true
2. 杀掉Agent，等40秒
3. `psql "SELECT online FROM agent_info WHERE ip_addr='127.0.0.1'"` → false
4. `psql "SELECT event_type, reason FROM agent_audit_log ORDER BY id DESC LIMIT 1"` → offline, "30s无心跳"
5. 重启Agent，等10秒
6. `psql "SELECT event_type FROM agent_audit_log ORDER BY id DESC LIMIT 1"` → online

### Step 3.7 `[ ]` 状态机完整实现与落库
- 封装 `updateTaskStatus(tid, newStatus, reason)` 函数：
  1. `BEGIN; SELECT * FROM hotmethod_task WHERE tid=? FOR UPDATE;`
  2. 校验 `old_status → new_status` 合法性（迁移图允许的边）
  3. 非法迁移 → ROLLBACK + 日志warning
  4. 合法 → `UPDATE status=?, status_info=? WHERE tid=?;`
  5. `INSERT INTO task_status_history(tid, old_status, new_status, reason, timestamp);`
  6. `COMMIT;`
- 迁移规则：
  - PENDING(0) → RUNNING(1), reason:"Agent开始执行采集"
  - RUNNING(1) → UPLOADING(2), reason:"采集完成，开始上传结果"
  - UPLOADING(2) → DONE(3), reason:"文件上传成功"
  - 任何 → FAILED(4), reason:具体错误消息
  - FAILED(4) → PENDING(0), reason:"用户触发重试"

**验收**：
1. 创建任务 → 查PG：status=0, status_info="任务已创建"
2. Agent拉到任务 → 查PG：status=1, status_info="Agent开始执行采集"
3. 采集完成上传 → 查PG：status=2, status_info="采集完成，开始上传结果"
4. 上传完成 → 查PG：status=3, status_info="文件上传成功"
5. `psql "SELECT old_status, new_status, reason FROM task_status_history WHERE tid='<tid>' ORDER BY timestamp"` → 按序显示4条迁移记录，每条reason非空
6. 手动调用 updateTaskStatus(tid, 3→1) → 日志warning "illegal transition DONE→RUNNING"，PG中status不变仍是3

---

## Phase 4：perf采集全链路 `[ ]`

**目标**：前端新建CPU采样 → Agent跑perf → 分析出火焰图 → 前端渲染

### Step 4.1 `[ ]` PerfProfiler采集器实现
- 继承 `IProfiler` 接口 `record()` / `collect_result()`
- `record()`: `fork+execvp("perf", {"perf", "record", "-F", hz, "-g", "-p", pid, "--", "sleep", duration})`
- **禁止 `system()` 调用**
- 子进程完成后 `waitpid` 回收

**验收**：
1. Agent收到CPU任务后，产出文件 `/tmp/drop_<tid>/perf.data`
2. `file /tmp/drop_<tid>/perf.data` 显示 "data" 类型
3. `ls -la /tmp/drop_<tid>/perf.data` 大小 > 0
4. 无僵尸进程：`ps aux | grep -c 'Z\|defunct'` → 0

### Step 4.2 `[ ]` 超时杀进程机制
- perf子进程启动前 `setpgid(0,0)` 独立进程组
- 看门狗线程：`sleep(timeoutSec)` 后 `killpg(pgid, SIGTERM)`
- 再等5秒：`killpg(pgid, SIGKILL)`
- `waitpid` 回收

**验收**：
1. 创建不存在的PID任务（会卡住的场景，或故意设很短的超时）
2. Agent日志出现 "task xxx timeout, sending SIGTERM"
3. 5秒后出现 "task xxx still alive, sending SIGKILL"
4. PG任务状态=FAILED，status_info含"timeout"
5. `ps aux | grep defunct` → 无僵尸

### Step 4.3 `[ ]` Agent上传MinIO + NotifyResult
- 采集完成后上传 `/tmp/drop_<tid>/perf.data` → `MinIO://drop-data/<tid>/perf.data`
- 调用 `Hotmethod.NotifyResult(TaskResult{taskID, errorMessage, cosKey})`

**验收**：
1. `docker compose exec minio mc ls local/drop-data/<tid>/` 看到 perf.data
2. PG中任务 status=3(DONE) 或 status_info 非空

### Step 4.4 `[ ]` analysis入口可被调起
- `python3 hotmethod_analyzer.py --task-id <tid> --task-type 0 --config config.ini`
- 从PG读任务参数，从MinIO下载perf.data
- 退出码0成功/非0失败，stderr写ErrorInfo

**验收**：
1. 预先在MinIO放一个perf.data，PG设status=DONE
2. 手动执行上述命令 → 退出码0
3. MinIO中出现产出文件

### Step 4.5 `[ ]` perf → 火焰图生成链
- `perf script -i perf.data > perf.script.txt`
- `stackcollapse-perf.pl perf.script.txt > collapsed.txt`
- `flamegraph.pl collapsed.txt > flamegraph.svg`

**验收**：
1. `docker compose exec minio mc ls local/drop-data/<tid>/` 看到 flamegraph.svg
2. `docker compose exec minio mc cat local/drop-data/<tid>/flamegraph.svg | head -1` → `<svg` 开头

### Step 4.6 `[ ]` TopN热点解析
- 解析 collapsed.txt → 按分号分割取最后一个函数为 self
- 统计每个函数的 value 总和 → 排序输出 top.json
- 格式：`[{"func":"main","self":1234,"inclusive":5678,"percentage":12.5}, ...]`

**验收**：
1. `docker compose exec minio mc cat local/drop-data/<tid>/top.json | jq '.[0].func'` → 非空字符串
2. `jq '.[0].percentage'` → 数值在0~100之间

### Step 4.7 `[ ]` apiserver定时轮询触发analysis
- 每10秒扫描 status=3(DONE) AND analysis_status=0(待分析)
- 调起analysis进程 → 更新 analysis_status=1(分析中)
- analysis脚本自行更新 analysis_status=2(成功)/3(失败)

**验收**：
1. 任务采集完成后等30秒
2. `psql "SELECT analysis_status FROM hotmethod_task WHERE tid='<tid>'"` → 2
3. MinIO中有完整的分析产物

### Step 4.8 `[ ]` web_frontend：任务列表+新建采样+任务详情
- `/index` 主页：Agent列表卡片 + 最近任务列表
- `/tasks` 全部任务：表格分页+搜索+状态标签颜色
- `/task/result?tid=xxx` 详情：基本信息 + Tab(火焰图/TopN/AI建议)
- 新建采样弹窗：Agent选择、采集类型下拉、PID、时长、频率

**验收**：
1. 浏览器访问主页能看到Agent列表（至少1个在线）
2. 点击"新建采样"→弹窗出现→填参数→提交成功
3. 任务列表能看到刚创建的任务，状态实时更新（轮询3秒）
4. 任务完成后点击进入详情页

### Step 4.9 `[ ]` 火焰图渲染
- MVP：analysis产出的SVG，前端用 `<iframe>` 加载PreSign URL
- 可选进阶：d3-flame-graph渲染JSON树，支持点击放大+搜索高亮

**验收**：
1. 详情页火焰图Tab → iframe加载SVG成功
2. 鼠标hover到栈帧 → 显示函数名和占比
3. PreSign URL有效（非403）

---

## Phase 5：eBPF采集器 `[ ]`

**目标**：eBPF采集器真跑，现场触发IO/调度异常能看到变化

### Step 5.1 `[ ]` 编写eBPF内核态探针
- off-CPU采样：`tracepoint/sched/sched_switch` 捕获被切换出的进程栈
- 或IO追踪：`kprobe/do_sys_open` / `tracepoint/syscalls/sys_enter_read`
- `.bpf.c` 中使用 `BPF_MAP_TYPE_STACK_TRACE` 采集内核栈+用户栈
- Makefile编译为 `.bpf.o`

**验收**：
1. `cd drop/common/bpf && make` 编译成功
2. `llvm-objdump -S offcpu.bpf.o | grep tracepoint` → 命中，确认探针存在
3. `llvm-objdump -S offcpu.bpf.o | grep -c BPF_MAP_TYPE_STACK_TRACE` → ≥1

### Step 5.2 `[ ]` 用户态libbpf加载器
- EbpfLoader类: `load(bpf_o_path)` → `attach(target_pid)` → `poll(duration_sec)` → `detach()` → `unload()`
- ring buffer 读取栈数据，结合 `/proc/kallsyms` 解析符号
- 转为折叠栈格式写入文件

**验收**：
1. 单元测试 `ctest -R ebpf` 退出码0
2. 手动加载offcpu.bpf.o → sleep 3秒 → detach → 输出折叠栈文件
3. `head output.txt | grep ';'` → 折叠栈格式正确（`func1;func2 ... count`）
4. `dmesg | tail -5` → 无 BPF verifier 错误

### Step 5.3 `[ ]` EbpfProfiler采集器类
- 继承 `IProfiler`
- `record()`: 启动EbpfLoader采样指定时长
- `collect_result()`: 返回折叠栈文件路径
- 注册到Agent工厂：profiler_type=3 → EbpfProfiler

**验收**：
1. 通过API创建 profiler_type=3 的任务
2. Agent日志："EbpfProfiler started for tid=xxx"
3. 采集完成后产物 `/tmp/drop_<tid>/collapsed_ebpf.txt` 非空
4. `head collapsed_ebpf.txt | grep ';'` → 格式正确

### Step 5.4 `[ ]` eBPF数据 → 火焰图
- analysis识别eBPF折叠栈 → `flamegraph.pl --color=io --title="Off-CPU Flame Graph"`
- 上传 `flamegraph_offcpu.svg` 到MinIO

**验收**：
1. `mc ls local/drop-data/<tid>/` 看到 flamegraph_offcpu.svg
2. SVG标题包含 "Off-CPU"
3. 颜色方案不同于CPU火焰图（IO色系，偏紫/绿）

### Step 5.5 `[ ]` Web端eBPF独有可视化
- 详情页根据profiler_type显示不同Tab
- eBPF → "Off-CPU火焰图" Tab
- 可选：IO延迟热力图（ECharts heatmap）

**验收**：
1. 创建eBPF任务 → 完成后详情页
2. 看到"Off-CPU火焰图"Tab而非"CPU火焰图"
3. 视觉上与CPU火焰图有明显区别

### Step 5.6 `[ ]` demo脚本验证eBPF
- 脚本1：`dd if=/dev/zero of=/tmp/bigfile bs=1M count=1024` 触发IO压力
- 脚本2：`stress-ng --cpu 4 --io 2 --timeout 30s` 触发调度竞争
- 在压力期间启动eBPF采集，采集后对比基线

**验收**：
1. 基线（无压力）off-CPU火焰图 → 主要在idle/sleep
2. 施压期间off-CPU火焰图 → 出现IO wait / schedule相关栈帧
3. 两次火焰图对比差异肉眼可辨，包含dd/stress相关调用

### Step 5.7 `[ ]` Docker特权配置
- drop_agent服务：`privileged: true`（或 `cap_add: [SYS_ADMIN, PERFMON, NET_ADMIN]`）
- `pid: host`（看到主机进程）
- 挂载 `/sys/kernel/debug:/sys/kernel/debug:ro`
- 挂载 `/sys/fs/bpf:/sys/fs/bpf`
- 注释说明每个权限的用途

**验收**：
1. `docker compose up -d`
2. `docker compose exec drop_agent ls /sys/fs/bpf` → BPF文件系统可访问
3. `docker compose exec drop_agent cat /proc/1/cmdline | tr '\0' ' '` → 能看到主机systemd或init
4. `docker compose exec drop_agent perf stat -p 1 sleep 0.1 2>&1` → 能执行perf，不报权限错误

---

## Phase 6：用户态语言级采集器 `[ ]`

**目标**：第二种采集器，不同栈语义，有独立可视化

### Step 6.1 `[ ]` 集成用户态采集器工具
- 推荐 async-profiler（Java）：预编译二进制放入 `drop/tools/async-profiler/`
- 或 py-spy（Python）：pip install py-spy
- 或 pprof HTTP（Go）：无需额外工具，curl直接采

**验收**：
1. `ls drop/tools/async-profiler/asprof` → 文件存在且有执行权限
2. 手动运行对目标进程采样 → 产出文件正常

### Step 6.2 `[ ]` 对应Profiler类实现
- `AsyncProfilerProfiler` / `PySpyProfiler` / `PprofProfiler`
- 继承 `IProfiler`，实现 `record()` + `collect_result()`
- 工厂注册：profiler_type=1(async-profiler) / 2(pprof)

**验收**：
1. 创建对应profiler_type的任务 → Agent日志显示正确Profiler启动
2. 采集完成 → 产物文件存在且非空

### Step 6.3 `[ ]` 折叠栈格式适配
- async-profiler JFR: analysis侧用jfr2collapsed转换
- py-spy: `py-spy record --format flamegraph` 直接输出SVG，或 `--format speedscope` 输出JSON
- pprof: `go tool pprof -raw` → 解析转折叠栈

**验收**：
1. analysis处理后 MinIO中出现对应格式的折叠栈文件
2. `head collapsed.txt | grep ';'` → 格式正确
3. 火焰图SVG正常生成

### Step 6.4 `[ ]` Web端独立可视化
- async-profiler: Java火焰图（`--color=java`色系，粉/橙色调）
- py-spy: Python火焰图，标注Python特有栈帧
- pprof: Go pprof图 + Top表格

**验收**：
1. 详情页根据profiler_type显示专属Tab名
2. 颜色/样式与CPU perf火焰图有视觉区别
3. 图表中能看到目标语言的典型函数名

### Step 6.5 `[ ]` 新建任务弹窗支持采集器选择
- 下拉框选项：CPU-perf / eBPF-offCPU / Java-async-profiler / Python-py-spy / Go-pprof
- 不同类型动态展示不同参数表单

**验收**：
1. 选择Java-async-profiler → 出现JDK路径输入框
2. 选择CPU-perf → 参数同之前
3. 提交后API接收的profiler_type值正确

---

## Phase 7：Continuous Profiling `[ ]`

**目标**：常驻低频采样 + 时间轴回溯任意5分钟窗口

### Step 7.1 `[ ]` Agent常驻低频采样模式
- 新增任务类型 continuous profiling
- Agent收到后启动常驻eBPF/perf采样，默认1Hz
- 数据保持在内存ring buffer中

**验收**：
1. 创建continuous任务 → Agent日志 "Continuous profiling mode started, hz=1"
2. 活动持续不退出，进程不崩溃

### Step 7.2 `[ ]` 定时切割保存
- 每5分钟将ring buffer折叠写入文件
- 上传MinIO：`/<tid>/<timestamp>/collapsed.txt`
- 写PG表 continuous_profile_segments(tid, start_ts, end_ts, s3_key)

**验收**：
1. 等10分钟
2. `psql "SELECT COUNT(*) FROM continuous_profile_segments WHERE tid='<tid>'"` → ≥2
3. `mc ls local/drop-data/<tid>/` → ≥2个时间戳目录

### Step 7.3 `[ ]` Web时间轴组件
- ECharts dataZoom slider显示时间范围
- 拖动选择5分钟区间
- 选中后请求合并API → 渲染火焰图

**验收**：
1. 任务详情页显示时间轴slider
2. 拖动选择一个区间 → 火焰图更新
3. 不同时段火焰图有差异（若负载有变化）

### Step 7.4 `[ ]` 窗口合并API
- `GET /api/v1/tasks/:tid/profile-window?start=<ts>&end=<ts>`
- 从MinIO下载该范围内的所有collapsed.txt
- 合并相同栈帧的value
- 返回合并折叠栈或直接生成的火焰图

**验收**：
1. `curl -s "http://localhost:8191/api/v1/tasks/<tid>/profile-window?start=...&end=..." | head` → 返回数据
2. 多段合并后计数 > 单段计数

### Step 7.5 `[ ]` apiserver定时任务管理
- `POST /api/v1/schedule/task` 创建定时规则
- 到时间自动下发continuous profiling任务

**验收**：
1. 创建定时规则 → 等下一个调度时间 → 新任务自动创建
2. Agent收到并开始常驻采样

---

## Phase 8：智能归因 + 自然语言采集 `[ ]`（加分项）

### Step 8.1 `[ ]` 规则建议引擎
- YAML规则文件：`rules.yaml` → `{regex, advice}`
- 匹配TopN函数名 → 输出 suggestions.md

**验收**：
1. 对有malloc/lock函数的数据运行 → suggestions.md包含对应建议
2. `mc cat local/drop-data/<tid>/suggestions.md | head` → 有内容

### Step 8.2 `[ ]` LLM归因封装
- 支持OpenAI兼容API
- 输入TopN JSON + 元数据
- 工具定义：LLM只能调用 `get_topn()`, `get_flamegraph()`, `get_task_meta()`
- 输出结构化JSON：`{diagnosis, evidence, recommendation, confidence}`

**验收**：
1. AI建议Tab显示归因结论包含所有4个字段
2. evidence字段引用具体函数名和占比
3. confidence在0-1之间

### Step 8.3 `[ ]` 自然语言入口
- 前端对话框
- 后端解析意图→提取PID/时间/采集类型
- 自动完成任务创建→采完总结→追问

**验收**：
1. 输入"过去一小时CPU飙高帮他看看" → 系统自动创建采样
2. 完成后返回文字总结

---

## Phase 9：测试加固 + 部署完善 `[ ]`

**目标**：质量达标，一台干净机器10分钟内一键跑通

### Step 9.1 `[ ]` C++单测覆盖 ≥50%
- 重点覆盖：状态机迁移逻辑、任务队列线程安全、折叠栈解析、PidStats计算
- `gcovr -r ..` 报告 ≥50%

**验收**：
1. `cd drop/build && cmake .. -DBUILD_TESTS=ON && make && ctest` 全过
2. `gcovr -r .. | tail -1` → 行覆盖率 ≥ 50%

### Step 9.2 `[ ]` Go单测覆盖 ≥50%
- 重点覆盖：12个handler、updateTaskStatus事务、Storage接口MinIO实现
- `go test -coverprofile=cover.out ./...`

**验收**：
1. `cd apiserver && go test ./...` 全过
2. `go tool cover -func=cover.out | tail -1` → ≥50%

### Step 9.3 `[ ]` Python单测覆盖 ≥50%
- 重点覆盖：折叠栈解析、TopN计算、规则匹配、storage接口
- `pytest --cov=. --cov-report=term-missing`

**验收**：
1. `cd analysis && pytest` 全过
2. 末尾输出 coverage ≥ 50%

### Step 9.4 `[ ]` 端到端集成测试① — 正常路径
- 脚本自动：创建CPU任务 → 轮询直到DONE → 下载SVG → 验证 `<svg` 开头

**验收**：
1. `make test-e2e-normal` 退出码 0
2. 脚本输出包含 "PASS: normal path"

### Step 9.5 `[ ]` 端到端集成测试② — 不存在PID
- 脚本：创建PID=999999任务 → 轮询直到FAILED → 检查status_info含"process not found"之类

**验收**：
1. `make test-e2e-invalid-pid` 退出码 0
2. PG中 status=4(FAILED), status_info 包含进程不存在相关错误

### Step 9.6 `[ ]` 端到端集成测试③ — 采集超时
- 脚本：创建duration=300秒任务 + 设置全局采集超时=10秒 → 超时杀进程 → FAILED

**验收**：
1. `make test-e2e-timeout` 退出码 0
2. Agent日志含 "timeout, sending SIGTERM"
3. PG中 status=4(FAILED), status_info 含 "timeout"

### Step 9.7 `[ ]` docker compose healthcheck完善
- 每个服务都有 healthcheck
- apiserver: `curl -f http://localhost:8191/healthz`
- drop_server: `grpcurl -plaintext localhost:50051 grpc.health.v1.Health/Check`

**验收**：
1. `docker compose up -d && sleep 15 && docker compose ps` → 所有服务 healthy

### Step 9.8 `[ ]` make demo脚本
- 等所有服务healthy
- 自动创建一个CPU采样任务
- 等完成
- 打印访问地址

**验收**：
1. 在干净环境（无残留容器）：`docker compose down -v && make demo`
2. 10分钟内输出 "Demo ready! Open http://localhost"
3. 浏览器可正常访问看到任务列表

### Step 9.9 `[ ]` 设计文档
- ≤10页A4（约300行Markdown）
- 必含：架构图、状态机迁移图、关键决策与取舍、AI协作章节、性能自证、"如果再有7天"

**验收**：
1. `wc -l docs/design.md` → ≤300
2. `grep -c "状态机" docs/design.md` → ≥1
3. `grep -c "取舍" docs/design.md` → ≥1

### Step 9.10 `[ ]` README
- 硬件要求：x86_64, ≥2GB RAM
- 内核要求：≥5.8（eBPF CAP_BPF）
- 权限要求：perf_event_paranoid, Docker privileged
- 快速开始：git clone → docker compose up → make demo

**验收**：
1. `grep -c "docker compose up" README.md` → ≥1
2. `grep -c "perf_event_paranoid" README.md` → ≥1
3. `grep -c "[5-9]\.[0-9]" README.md` → ≥1（内核版本要求）

---

## 依赖关系图

```
Phase 1 ──→ Phase 2 ──→ Phase 4 ──→ Phase 6
   │            │           │
   └──→ Phase 3 ──→ Phase 5 ──→ Phase 7
                                        │
                                   Phase 8 (可选)
                                        │
                                   Phase 9
```

**关键路径**：Phase 1 → 2 → 3 → 4 → 5 → 9（最短可交付路径）

## 需求映射速查

| Phase | 覆盖需求编号 |
|-------|-------------|
| 1 | 基础设施（proto, PG, MinIO, 日志） |
| 2 | B4(状态机), B5(心跳+审计日志), G1(docker骨架) |
| 3 | 无新需求（Phase 2的延续验证） |
| 4 | B1(Web下发), B2(Agent采集+通知), B3(火焰图渲染) |
| 5 | E2(eBPF采集器), G3(eBPF真跑) |
| 6 | E3(用户态采集器) |
| 7 | E1(Continuous Profiling) |
| 8 | E4(智能归因), E5(自然语言) |
| 9 | B6(单测≥50%+≥3个e2e), G1(make demo), G2(提交规范), G4(设计文档) |
