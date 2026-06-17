---
name: activeContext
description: 当前vibecoding会话的聚焦点、正在做的事、下一步行动
type: project
---

# 活跃上下文

## 当前聚焦

- **组件**：全组件
- **Phase**：Phase 8 ✅ 已完成
- **正在做**：无
- **下一步**：Phase 9 测试加固+部署

## 当前阻塞

- async-profiler 二进制因网络SSL问题无法在当前环境下载（Dockerfile脚本有fallback，构建时通常可用）

## 本次会话摘要

- 完成 Phase 8 智能归因+自然语言采集 全部3步：
  - Step 8.1: 创建 rules.yaml 规则配置（16条常见性能规则）+ analysis_advisor.py 规则引擎（加载YAML、正则匹配TopN函数、生成suggestions.md）
  - Step 8.2: 实现 llm_client（OpenAI兼容API封装、工具定义、结构化输出diagnosis/evidence/recommendation/confidence、fallback回退）+ 集成到hotmethod_analyzer.py（分析后自动生成suggestions.md+ai_suggestion.md→上传MinIO→写入analysis_suggestion表）+ apiserver新增 GET /tasks/:tid/suggestions 接口
  - Step 8.3: 实现 nl_parser.py（从NL提取PID/profiler_type/time_range/duration/target_ip）+ apiserver/server/nl_handler.go（NL聊天处理、自动创建任务、SSE流式推送进度）+ 前端AI建议面板+NL对话框
  - 31个新Python测试全过，6个新Go测试全过（含SSE、NL fallback），前端构建成功
