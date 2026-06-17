package server

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"github.com/drop/apiserver/model"
	"github.com/gin-gonic/gin"
	"go.uber.org/zap"
)

// NLChatReq is the request body for natural language chat.
type NLChatReq struct {
	Message string `json:"message" binding:"required"`
	TID     string `json:"tid,omitempty"` // optional: existing task context
}

// NLChatResp is the response for natural language chat.
type NLChatResp struct {
	Type    string      `json:"type"`    // "text", "task_created", "task_summary"
	Content interface{} `json:"content"` // string or structured object
	TID     string      `json:"tid,omitempty"`
}

// NLChat handles natural language profiling requests.
// It parses intent, optionally creates a task, and streams back the result.
func (s *APIServer) NLChat(c *gin.Context) {
	var req NLChatReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000200, "message": err.Error()})
		return
	}

	uid := c.GetString("uid")
	userName := c.GetString("user_name")

	// 1. Parse intent via Python nl_parser
	agents := s.getAvailableAgents()
	agentsJSON, _ := json.Marshal(agents)
	intent, err := s.parseNLIntent(req.Message, string(agentsJSON))
	if err != nil {
		s.logger.Error("NL parse failed", zap.Error(err))
		s.nlTextReply(c, "抱歉，我无法理解您的请求。请尝试描述您想分析的进程和问题，例如：\"过去一小时127.0.0.1上PID为1234的CPU飙高\"")
		return
	}

	s.logger.Info("NL intent parsed",
		zap.String("raw", req.Message),
		zap.Any("intent", intent),
	)

	// 2. Check if user wants to just analyze existing data vs create new task
	if req.TID != "" {
		// Context mode: answer questions about an existing task
		s.nlAnswerAboutTask(c, req.TID, req.Message)
		return
	}

	// 3. Create a new profiling task
	pid, _ := intent["pid"].(float64)
	profilerType, _ := intent["profiler_type"].(float64)
	duration, _ := intent["duration"].(float64)
	targetIP, _ := intent["target_ip"].(string)

	if targetIP == "" {
		targetIP = "127.0.0.1"
	}
	if duration == 0 {
		duration = 10
	}

	tid := genTID()
	taskName := fmt.Sprintf("NL: %s", truncate(req.Message, 100))

	task := model.HotmethodTask{
		TID:           tid,
		Name:          taskName,
		Type:          model.TaskTypeSingle,
		ProfilerType:  int(profilerType),
		TargetIP:      targetIP,
		RequestParams: fmt.Sprintf(`{"pid":%d,"duration":%d,"hz":99,"event":"cpu"}`, int(pid), int(duration)),
		Status:        model.TaskStatusPending,
		StatusInfo:     "自然语言触发，任务已创建",
		UID:           uid,
		UserName:      userName,
		CreateTime:    model.Now(),
	}
	if err := s.db.Create(&task).Error; err != nil {
		s.logger.Error("create NL task", zap.Error(err))
		s.nlTextReply(c, "创建采样任务失败，请稍后重试。")
		return
	}

	// Insert status history
	history := model.TaskStatusHistory{
		TID:       tid,
		OldStatus: -1,
		NewStatus: model.TaskStatusPending,
		Reason:    "自然语言触发，任务已创建",
		Timestamp: task.CreateTime,
	}
	s.db.Create(&history)

	// Dispatch the task
	if s.grpcCC != nil {
		createReq := CreateTaskReq{
			Type:         model.TaskTypeSingle,
			ProfilerType: int(profilerType),
			TargetIP:     targetIP,
			PID:          int(pid),
			Duration:     int(duration),
			Hz:           99,
		}
		go s.dispatchTask(tid, createReq)
	}

	// 4. Respond with task created info
	profilerName := map[int]string{0: "CPU-perf", 1: "Java-async-profiler", 2: "Go-pprof", 3: "eBPF-offCPU"}
	pName := profilerName[int(profilerType)]
	if pName == "" {
		pName = "CPU-perf"
	}

	reply := fmt.Sprintf("好的！已为您创建采样任务：\n"+
		"- **任务ID**: %s\n"+
		"- **采集类型**: %s\n"+
		"- **目标**: %s (PID: %d)\n"+
		"- **时长**: %d秒\n\n"+
		"任务正在下发到Agent执行，完成后我会为您生成分析报告。",
		tid, pName, targetIP, int(pid), int(duration))

	c.JSON(http.StatusOK, gin.H{
		"code": 0,
		"data": NLChatResp{
			Type:    "task_created",
			Content: reply,
			TID:     tid,
		},
	})
}

// NLChatFollowup handles follow-up questions about a task's analysis.
func (s *APIServer) NLChatFollowup(c *gin.Context) {
	var req NLChatReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000201, "message": err.Error()})
		return
	}

	tid := c.Param("tid")
	s.nlAnswerAboutTask(c, tid, req.Message)
}

// nlTextReply sends a simple text reply.
func (s *APIServer) nlTextReply(c *gin.Context, text string) {
	c.JSON(http.StatusOK, gin.H{
		"code": 0,
		"data": NLChatResp{
			Type:    "text",
			Content: text,
		},
	})
}

// nlAnswerAboutTask answers questions about a completed task using AI suggestions.
func (s *APIServer) nlAnswerAboutTask(c *gin.Context, tid string, question string) {
	// Get task info
	var task model.HotmethodTask
	if err := s.db.Where("tid = ?", tid).First(&task).Error; err != nil {
		s.nlTextReply(c, "找不到该任务。")
		return
	}

	// Get suggestions
	var suggestions []model.AnalysisSuggestion
	s.db.Where("tid = ?", tid).Find(&suggestions)

	// Build context
	var ruleSugs []map[string]interface{}
	var aiSug map[string]interface{}
	for _, sug := range suggestions {
		if sug.Status == 0 { // rule-based
			ruleSugs = append(ruleSugs, map[string]interface{}{
				"func":       sug.Func,
				"suggestion": sug.Suggestion,
			})
		} else if sug.Status == 1 && sug.Func == "__ai_summary__" { // AI
			var parsed map[string]interface{}
			if err := json.Unmarshal([]byte(sug.AISuggestion), &parsed); err == nil {
				aiSug = parsed
			}
		}
	}

	// If AI suggestion exists, use it as the basis for answering
	if aiSug != nil {
		answer := fmt.Sprintf("## 分析报告 (任务 %s)\n\n", tid)
		if diag, ok := aiSug["diagnosis"].(string); ok {
			answer += fmt.Sprintf("**诊断**: %s\n\n", diag)
		}
		if evidence, ok := aiSug["evidence"].(string); ok {
			answer += fmt.Sprintf("**证据**: %s\n\n", evidence)
		}
		if rec, ok := aiSug["recommendation"].(string); ok {
			answer += fmt.Sprintf("**建议**: %s\n\n", rec)
		}
		if conf, ok := aiSug["confidence"].(float64); ok {
			answer += fmt.Sprintf("**置信度**: %.0f%%\n", conf*100)
		}
		c.JSON(http.StatusOK, gin.H{
			"code": 0,
			"data": NLChatResp{
				Type:    "task_summary",
				Content: answer,
				TID:     tid,
			},
		})
		return
	}

	// Fallback to rule suggestions
	if len(ruleSugs) > 0 {
		answer := fmt.Sprintf("## 规则建议 (任务 %s)\n\n", tid)
		for i, rs := range ruleSugs {
			answer += fmt.Sprintf("%d. **%s**: %s\n", i+1, rs["func"], rs["suggestion"])
		}
		c.JSON(http.StatusOK, gin.H{
			"code": 0,
			"data": NLChatResp{
				Type:    "task_summary",
				Content: answer,
				TID:     tid,
			},
		})
		return
	}

	// Check task status
	statusMsg := map[int]string{
		0: "任务等待下发中",
		1: "任务正在执行采集",
		2: "任务正在上传数据",
		3: "任务已完成，分析进行中",
		4: "任务执行失败",
	}
	msg := statusMsg[task.Status]
	if msg == "" {
		msg = "任务状态未知"
	}
	s.nlTextReply(c, fmt.Sprintf("任务 %s 当前状态: %s。%s", tid, msg,
		map[string]string{
			"0": "请稍等任务执行完成。",
			"1": "请稍等采集完成。",
			"2": "数据上传中，请稍等。",
			"3": "分析正在生成中，请稍等片刻。",
			"4": "任务执行失败，您可以重试。",
		}[strconv.Itoa(task.Status)]))
}

// parseNLIntent calls the Python nl_parser to extract structured intent.
func (s *APIServer) parseNLIntent(text string, agentsJSON string) (map[string]interface{}, error) {
	// Try Python first
	cmd := exec.Command("python3",
		"/root/Drop/analysis/nl_parser.py",
		text,
		"--agents", agentsJSON,
	)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	cmd.Dir = "/root/Drop/analysis"

	if err := cmd.Run(); err != nil {
		s.logger.Warn("nl_parser.py failed, using Go fallback",
			zap.Error(err),
			zap.String("stderr", stderr.String()),
		)
		return s.goNLFallback(text), nil
	}

	var result map[string]interface{}
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		s.logger.Warn("nl_parser output invalid JSON, using Go fallback", zap.Error(err))
		return s.goNLFallback(text), nil
	}
	return result, nil
}

// goNLFallback is a simple Go-based NL parser when Python is unavailable.
func (s *APIServer) goNLFallback(text string) map[string]interface{} {
	result := map[string]interface{}{
		"pid":           0,
		"profiler_type": 0,
		"duration":      10,
		"target_ip":     "127.0.0.1",
		"raw_text":      text,
	}

	// Simple keyword matching
	lower := strings.ToLower(text)
	if strings.Contains(lower, "ebpf") || strings.Contains(lower, "off-cpu") ||
		strings.Contains(lower, "阻塞") || strings.Contains(lower, "io") {
		result["profiler_type"] = 3
	} else if strings.Contains(lower, "java") || strings.Contains(lower, "jvm") {
		result["profiler_type"] = 1
	}

	// Extract duration
	for _, suffix := range []string{"秒", "秒钟"} {
		if idx := strings.Index(text, suffix); idx > 0 {
			numStart := idx - 1
			for numStart > 0 && text[numStart-1] >= '0' && text[numStart-1] <= '9' {
				numStart--
			}
			if dur, err := strconv.Atoi(text[numStart:idx]); err == nil && dur > 0 {
				result["duration"] = dur
			}
		}
	}

	// Extract PID
	for _, prefix := range []string{"PID", "pid", "进程"} {
		if idx := strings.Index(text, prefix); idx >= 0 {
			rest := text[idx+len(prefix):]
			rest = strings.TrimLeft(rest, "是为：: ")
			numEnd := 0
			for numEnd < len(rest) && rest[numEnd] >= '0' && rest[numEnd] <= '9' {
				numEnd++
			}
			if numEnd > 0 {
				if pid, err := strconv.Atoi(rest[:numEnd]); err == nil {
					result["pid"] = pid
				}
			}
		}
	}

	return result
}

// getAvailableAgents returns list of online agents from PG.
func (s *APIServer) getAvailableAgents() []map[string]string {
	var agents []model.AgentInfo
	s.db.Where("online = ?", true).Find(&agents)

	result := make([]map[string]string, 0, len(agents))
	for _, a := range agents {
		result = append(result, map[string]string{
			"ip_addr":  a.IPAddr,
			"hostname": a.Hostname,
		})
	}
	if len(result) == 0 {
		result = append(result, map[string]string{"ip_addr": "127.0.0.1", "hostname": "localhost"})
	}
	return result
}

// NLStreamSSE handles Server-Sent Events for streaming NL chat results.
// For MVP, we poll the task status and send updates until analysis is done.
func (s *APIServer) NLStreamSSE(c *gin.Context) {
	tid := c.Param("tid")

	c.Header("Content-Type", "text/event-stream")
	c.Header("Cache-Control", "no-cache")
	c.Header("Connection", "keep-alive")

	ticker := time.NewTicker(3 * time.Second)
	defer ticker.Stop()
	start := time.Now()
	timeout := 5 * time.Minute

	for {
		select {
		case <-c.Request.Context().Done():
			return
		case <-ticker.C:
			if time.Since(start) > timeout {
				s.sseSend(c, "error", "timeout waiting for analysis")
				return
			}

			var task model.HotmethodTask
			if err := s.db.Where("tid = ?", tid).First(&task).Error; err != nil {
				s.sseSend(c, "error", "task not found")
				return
			}

			// Send status update
			s.sseSend(c, "status", map[string]interface{}{
				"status":         task.Status,
				"status_info":    task.StatusInfo,
				"analysis_status": task.AnalysisStatus,
			})

			// If analysis is complete, send final result and close
			if task.AnalysisStatus == model.AnalysisStatusSuccess {
				// Get suggestions
				var suggestions []model.AnalysisSuggestion
				s.db.Where("tid = ?", tid).Find(&suggestions)
				s.sseSend(c, "complete", suggestions)
				return
			}

			if task.AnalysisStatus == model.AnalysisStatusFailed {
				s.sseSend(c, "error", "analysis failed: "+task.StatusInfo)
				return
			}

			if task.Status == model.TaskStatusFailed {
				s.sseSend(c, "error", "task failed: "+task.StatusInfo)
				return
			}

			// Flush
			if f, ok := c.Writer.(http.Flusher); ok {
				f.Flush()
			}
		}
	}
}

func (s *APIServer) sseSend(c *gin.Context, event string, data interface{}) {
	jsonData, _ := json.Marshal(data)
	fmt.Fprintf(c.Writer, "event: %s\ndata: %s\n\n", event, string(jsonData))
	if f, ok := c.Writer.(http.Flusher); ok {
		f.Flush()
	}
}

func truncate(s string, maxLen int) string {
	if len(s) <= maxLen {
		return s
	}
	return s[:maxLen]
}

// Suppress unused import warning
var _ = io.EOF
