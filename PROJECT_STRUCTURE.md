# Drop 项目结构

```
Drop/
├── DROP产品文档.md                 # 产品规格锚定文档（只读参考）
├── CODEBUDDY.md                    # AI协作规范
├── docker-compose.yml              # 一键部署
├── Makefile                        # make proto / build / test / demo
├── memory-bank/                    # 进度记忆库（跨会话持久）
│   ├── projectbrief.md             #   项目定位与硬性约束
│   ├── architecture.md             #   架构决策与数据流
│   ├── techContext.md              #   技术栈与版本锁定
│   ├── implementation-plan.md      #   Vibecoding实现顺序
│   ├── requirements-traceability.md#   需求→实现追踪矩阵
│   ├── progress.md                 #   实时进度与测试覆盖率
│   └── activeContext.md            #   当前会话聚焦点
│
├── drop/                           # C++ Agent + Server
│   ├── CMakeLists.txt
│   ├── common/
│   │   ├── proto/                  # 5个.proto协议定义
│   │   │   ├── common.proto        #   PidStats/File/CosConfig/RecordArgv
│   │   │   ├── healthcheck.proto   #   Agent心跳+任务拉取
│   │   │   ├── hotmethod.proto     #   任务下发+结果上报
│   │   │   ├── control.proto       #   apiserver→Server控制面
│   │   │   └── init.proto          #   Agent注册+配置拉取
│   │   ├── src/
│   │   │   ├── task_queue.h          #   TaskQueue+HeartbeatTracker+PGStore
│   │   │   ├── task_queue.cpp        #   PG连通+upsert+状态迁移+审计日志
│   │   │   └── bpf/                    #   eBPF内核态程序
│   │   │       ├── offcpu.bpf.c        #   off-CPU探针
│   │   │       ├── iosnoop.bpf.c       #   IO延迟探针
│   │   │       └── vmlinux.h           #   内核类型定义
│   ├── agent/
│   │   └── main.cpp                #   入口: 心跳线程+工作线程+守护化+PidStats
│   ├── server/
│   │   └── main.cpp                #   启动4个gRPC service+心跳扫描线程
│   ├── client/                     # Python调试CLI（开发用）
│   ├── etc/
│   │   ├── config.json.example
│   │   └── certs/
│   ├── tools/                      # 第三方二进制(perf/async-profiler等)
│   └── tests/
│
├── apiserver/                      # Go HTTP后端
│   ├── main.go
│   ├── go.mod
│   ├── go.sum
│   ├── config/
│   │   └── config.go               #   Viper配置加载
│   ├── server/
│   ├── server/
│   │   ├── server.go               #   APIServer+状态机+updateTaskStatus
│   │   └── handler.go              #   12个handler+gRPC dispatchTask
│   ├── model/
│   │   └── model.go               #   GORM 9表+常量定义
│   ├── middleware/
│   │   ├── auth.go                 #   Cookie鉴权+dev-mode
│   │   ├── cors.go                 #   CORS
│   │   ├── log.go                  #   结构化access log
│   │   └── ratelimit.go            #   限流
│   ├── proto/                      #   复制自drop/common/proto
│   ├── pkg/
│   │   ├── storage/
│   │   │   ├── storage.go          #   interface Storage
│   │   │   ├── cos/                #   COS实现
│   │   │   └── minio/              #   MinIO实现
│   │   └── util/
│   ├── apiserver.yaml.example
│   └── tests/
│
├── analysis/                       # Python分析引擎
│   ├── hotmethod_analyzer.py       #   总入口
│   ├── storage.py                  #   Storage/COSStorage/MinIOStorage
│   ├── config.ini.example
│   ├── data_parser/
│   │   ├── collapsed_data_parser.py#   折叠栈解析
│   │   ├── pprof_data_parser.py    #   pprof解析
│   │   └── pprof_heap_parser.py    #   pprof heap解析
│   ├── flamegraph.py               #   火焰图生成
│   ├── flamegraph.pl               #   Perl火焰图脚本
│   ├── stackcollapse-perf.pl
│   ├── hotmethod_common.py         #   TopN计算
│   ├── memleak_analyzer.py         #   内存泄漏分析
│   ├── analysis_advisor.py         #   规则建议引擎
│   ├── ai_advisor.py               #   LLM归因封装
│   ├── resource_analyzer.py        #   资源曲线分析
│   ├── biotrace.py                 #   eBPF IO分析
│   ├── drop_analyzer/
│   │   └── assembly_code_analyzer/
│   │       └── assembly_code_analyzer.py
│   ├── java_heap_analyzer/         #   Go子项目
│   │   ├── main.go
│   │   └── go.mod
│   ├── Dockerfile
│   ├── requirements.txt
│   └── tests/
│
├── web_frontend/                   # React SPA
│   ├── package.json
│   ├── tsconfig.json
│   ├── craco.config.js             #   TDesign主题覆盖
│   ├── public/
│   │   ├── index.html
│   │   └── config/
│   │       └── config.js           #   运行时配置(HOST_URL)
│   ├── src/
│   │   ├── index.tsx
│   │   ├── App.tsx
│   │   ├── router.tsx
│   │   ├── api/
│   │   │   └── index.ts            #   axios实例 + 接口函数
│   │   ├── store/
│   │   │   └── index.ts            #   Zustand store
│   │   ├── pages/
│   │   │   ├── login/
│   │   │   ├── home/               #   /index Agent列表+任务列表
│   │   │   ├── taskList/           #   /tasks 全部任务
│   │   │   └── taskResult/         #   /task/result 详情+火焰图
│   │   └── components/
│   │       ├── header/
│   │       ├── flamegraph/         #   d3-flame-graph封装
│   │       ├── createTaskModal/    #   新建采样弹窗
│   │       └── timeline/           #   时间轴(Continuous Profiling)
│   ├── Dockerfile
│   ├── nginx.conf
│   └── tests/
│
├── docs/
│   └── design.md                   # ≤10页设计文档
│
└── .github/
    └── workflows/
        └── ci.yml                  # CI: proto生成 + 编译 + 单测 + e2e
```

---
