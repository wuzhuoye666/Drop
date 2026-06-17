package server

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"math/rand"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/drop/apiserver/model"
	pb "github.com/drop/apiserver/proto"
	"github.com/gin-gonic/gin"
	"go.uber.org/zap"
)

// ==================== Request Types ====================

type CreateTaskReq struct {
	Name         string `json:"name"`
	Type         int    `json:"type"`
	ProfilerType int    `json:"profiler_type"`
	TargetIP     string `json:"target_ip"`
	PID          int    `json:"pid"`
	Duration     int    `json:"duration"` // seconds
	Hz           int    `json:"hz"`       // sample rate
	Event        string `json:"event"`    // async-profiler event: cpu/alloc/lock/wall
}

type CreateGroupReq struct {
	Name string `json:"name"`
}

type AddMemberReq struct {
	Gid uint   `json:"gid"`
	UID string `json:"uid"`
}

type AddGroupAgentReq struct {
	Gid     uint   `json:"gid"`
	AgentID uint   `json:"agent_id"`
}

type CreateScheduleReq struct {
	Name         string `json:"name"`
	CronExpr     string `json:"cron_expr"`
	Type         int    `json:"type"`
	ProfilerType int    `json:"profiler_type"`
	TargetIP     string `json:"target_ip"`
	PID          int    `json:"pid"`
	Duration     int    `json:"duration"`
	Hz           int    `json:"hz"`
}

type CreateSegmentReq struct {
	StartTs int64  `json:"start_ts"`
	EndTs   int64  `json:"end_ts"`
	S3Key   string `json:"s3_key"`
}

type ProfileWindowReq struct {
	Start int64 `form:"start"`
	End   int64 `form:"end"`
}

// ==================== Auth ====================

func (s *APIServer) AuthCheck(c *gin.Context) {
	uid, _ := c.Cookie("drop_user_uid")
	if uid == "" && !s.cfg.Server.DevMode {
		c.JSON(http.StatusUnauthorized, gin.H{
			"code": 4010001,
			"data": gin.H{"location": "/login"},
		})
		return
	}
	if uid == "" {
		uid = "dev-uid"
	}
	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"uid": uid}})
}

// ==================== User ====================

func (s *APIServer) GetUser(c *gin.Context) {
	uid := c.GetString("uid")
	name := c.GetString("user_name")

	var user model.UserInfo
	err := s.db.Where("uid = ?", uid).First(&user).Error
	if err != nil {
		user = model.UserInfo{UID: uid, Name: name, Groups: "[]"}
		if err := s.db.Create(&user).Error; err != nil {
			s.logger.Error("create user", zap.Error(err))
			c.JSON(http.StatusInternalServerError, gin.H{"code": 5000001, "message": "create user failed"})
			return
		}
	}
	c.JSON(http.StatusOK, gin.H{"code": 0, "data": user})
}

// ==================== Tasks ====================

func (s *APIServer) CreateTask(c *gin.Context) {
	var req CreateTaskReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000001, "message": err.Error()})
		return
	}

	uid := c.GetString("uid")
	userName := c.GetString("user_name")
	tid := genTID()

	params, _ := json.Marshal(req)
	task := model.HotmethodTask{
		TID:           tid,
		Name:          req.Name,
		Type:          req.Type,
		ProfilerType:  req.ProfilerType,
		TargetIP:      req.TargetIP,
		RequestParams: string(params),
		Status:        model.TaskStatusPending,
		StatusInfo:    "任务已创建",
		UID:           uid,
		UserName:      userName,
		CreateTime:    model.Now(),
	}

	if err := s.db.Create(&task).Error; err != nil {
		s.logger.Error("create task", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000002, "message": "create task failed"})
		return
	}

	// Insert initial status history
	history := model.TaskStatusHistory{
		TID:       tid,
		OldStatus: -1,
		NewStatus: model.TaskStatusPending,
		Reason:    "任务已创建",
		Timestamp: task.CreateTime,
	}
	s.db.Create(&history)

	// Dispatch via gRPC to drop_server
	if s.grpcCC != nil {
		go s.dispatchTask(tid, req)
	} else {
		reason := "gRPC connection unavailable, cannot dispatch task to drop_server"
		s.logger.Warn("cannot dispatch task", zap.String("tid", tid), zap.String("reason", reason))
		if err := s.updateTaskStatus(context.Background(), tid, model.TaskStatusFailed, reason); err != nil {
			s.logger.Error("mark task failed on dispatch failure", zap.Error(err))
		}
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"tid": tid}})
}

func (s *APIServer) ListTasks(c *gin.Context) {
	uid := c.GetString("uid")
	page, _ := strconv.Atoi(c.DefaultQuery("page", "1"))
	pageSize, _ := strconv.Atoi(c.DefaultQuery("page_size", "20"))
	if pageSize > 100 {
		pageSize = 100
	}
	if page < 1 {
		page = 1
	}

	var total int64
	var tasks []model.HotmethodTask

	query := s.db.Model(&model.HotmethodTask{}).
		Where("uid = ? OR uid IN (SELECT uid FROM group_members WHERE gid IN (SELECT gid FROM group_members WHERE uid = ?))", uid, uid)

	query.Count(&total)
	query.Order("create_time DESC").
		Offset((page - 1) * pageSize).
		Limit(pageSize).
		Find(&tasks)

	c.JSON(http.StatusOK, gin.H{
		"code": 0,
		"data": gin.H{
			"list":  tasks,
			"total": total,
			"page":  page,
		},
	})
}

func (s *APIServer) GetTask(c *gin.Context) {
	tid := c.Param("tid")
	uid := c.GetString("uid")

	var task model.HotmethodTask
	if err := s.db.Where("tid = ? AND (uid = ? OR uid IN (SELECT uid FROM group_members WHERE gid IN (SELECT gid FROM group_members WHERE uid = ?)))", tid, uid, uid).
		First(&task).Error; err != nil {
		c.JSON(http.StatusNotFound, gin.H{"code": 4040001, "message": "task not found"})
		return
	}

	data := gin.H{
		"task": task,
	}
	if s.storage != nil {
		ctx := context.Background()
		keys, err := s.storage.ListObjects(ctx, tid+"/")
		if err == nil && len(keys) > 0 {
			files := make([]gin.H, 0, len(keys))
			for _, key := range keys {
				url, _ := s.storage.PreSign(ctx, key)
				name := key[strings.LastIndex(key, "/")+1:]
				files = append(files, gin.H{"name": name, "key": key, "url": url})
			}
			data["files"] = files
		}
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": data})
}

func (s *APIServer) DeleteTask(c *gin.Context) {
	tid := c.Param("tid")
	uid := c.GetString("uid")

	result := s.db.Where("tid = ? AND (uid = ? OR uid IN (SELECT uid FROM group_members WHERE gid IN (SELECT gid FROM group_members WHERE uid = ?)))", tid, uid, uid).Delete(&model.HotmethodTask{})
	if result.RowsAffected == 0 {
		c.JSON(http.StatusNotFound, gin.H{"code": 4040002, "message": "task not found or no permission"})
		return
	}

	s.db.Where("tid = ?", tid).Delete(&model.AnalysisSuggestion{})

	if s.storage != nil {
		ctx := context.Background()
		keys, _ := s.storage.ListObjects(ctx, tid+"/")
		for _, key := range keys {
			s.storage.Delete(ctx, key)
		}
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"tid": tid}})
}

func (s *APIServer) RetryTask(c *gin.Context) {
	tid := c.Param("tid")
	uid := c.GetString("uid")

	var oldTask model.HotmethodTask
	if err := s.db.Where("tid = ? AND uid = ? AND status = ?", tid, uid, model.TaskStatusFailed).
		First(&oldTask).Error; err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000002, "message": "only failed tasks can be retried"})
		return
	}

	if err := s.updateTaskStatus(context.Background(), tid, model.TaskStatusPending, "用户触发重试"); err != nil {
		s.logger.Error("retry task status update", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000003, "message": err.Error()})
		return
	}

	// Re-dispatch the task
	if s.grpcCC != nil {
		var req CreateTaskReq
		json.Unmarshal([]byte(oldTask.RequestParams), &req)
		go s.dispatchTask(tid, req)
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"tid": tid}})
}

// ==================== Agents ====================

func (s *APIServer) ListAgents(c *gin.Context) {
	uid := c.GetString("uid")

	var agents []model.AgentInfo
	s.db.Where("uid = ? OR uid IN (SELECT uid FROM group_members WHERE gid IN (SELECT gid FROM group_members WHERE uid = ?))", uid, uid).
		Find(&agents)

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": agents})
}

func (s *APIServer) StatAgent(c *gin.Context) {
	ip := c.Query("target_ip")

	// Try gRPC proxy to drop_server for real-time data
	if s.grpcCC != nil {
		client := pb.NewControlClient(s.grpcCC)
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		resp, err := client.StatAgent(ctx, &pb.StatAgentRequest{TargetIp: ip})
		if err == nil {
			agents := make([]gin.H, 0, len(resp.Agents))
			for _, a := range resp.Agents {
				agents = append(agents, gin.H{
					"hostname": a.HostName,
					"ip_addr":  a.IpAddr,
					"online":   a.Online,
					"uid":      a.Uid,
					"version":  a.AgentVersion,
				})
			}
			c.JSON(http.StatusOK, gin.H{"code": 0, "data": agents})
			return
		}
		s.logger.Warn("StatAgent gRPC failed, falling back to PG", zap.Error(err))
	}

	// Fallback to PG
	var agents []model.AgentInfo
	if ip != "" {
		s.db.Where("ip_addr = ?", ip).Find(&agents)
	} else {
		s.db.Find(&agents)
	}
	c.JSON(http.StatusOK, gin.H{"code": 0, "data": agents})
}

// ==================== COS Files ====================

func (s *APIServer) ListCosFiles(c *gin.Context) {
	tid := c.Query("tid")
	if tid == "" {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000004, "message": "tid required"})
		return
	}

	if s.storage == nil {
		c.JSON(http.StatusServiceUnavailable, gin.H{"code": 5030001, "message": "storage not configured"})
		return
	}

	ctx := context.Background()
	keys, err := s.storage.ListObjects(ctx, tid+"/")
	if err != nil {
		s.logger.Error("list cos files", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000004, "message": "list files failed"})
		return
	}

	files := make([]gin.H, 0, len(keys))
	for _, key := range keys {
		url, _ := s.storage.PreSign(ctx, key)
		name := key[strings.LastIndex(key, "/")+1:]
		files = append(files, gin.H{"name": name, "key": key, "url": url})
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": files})
}

// ==================== Groups ====================

func (s *APIServer) CreateGroup(c *gin.Context) {
	var req CreateGroupReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000005, "message": err.Error()})
		return
	}

	uid := c.GetString("uid")
	group := model.Group{Name: req.Name, OwnerID: uid}
	if err := s.db.Create(&group).Error; err != nil {
		s.logger.Error("create group", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000005, "message": "create group failed"})
		return
	}

	s.db.Create(&model.GroupMember{Gid: group.Gid, UID: uid})

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": group})
}

func (s *APIServer) ListGroups(c *gin.Context) {
	uid := c.GetString("uid")

	var gids []uint
	s.db.Model(&model.GroupMember{}).Where("uid = ?", uid).Pluck("gid", &gids)

	var groups []model.Group
	if len(gids) > 0 {
		s.db.Where("gid IN ?", gids).Find(&groups)
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": groups})
}

func (s *APIServer) AddGroupMember(c *gin.Context) {
	var req AddMemberReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000006, "message": err.Error()})
		return
	}

	member := model.GroupMember{Gid: req.Gid, UID: req.UID}
	if err := s.db.FirstOrCreate(&member, &model.GroupMember{Gid: req.Gid, UID: req.UID}).Error; err != nil {
		s.logger.Error("add group member", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000006, "message": "add member failed"})
		return
	}

	c.JSON(http.StatusOK, gin.H{"code": 0})
}

func (s *APIServer) RemoveGroupMember(c *gin.Context) {
	var req AddMemberReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000007, "message": err.Error()})
		return
	}

	s.db.Where("gid = ? AND uid = ?", req.Gid, req.UID).Delete(&model.GroupMember{})
	c.JSON(http.StatusOK, gin.H{"code": 0})
}

func (s *APIServer) AddGroupAgent(c *gin.Context) {
	var req AddGroupAgentReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000008, "message": err.Error()})
		return
	}

	s.db.Model(&model.AgentInfo{}).Where("id = ?", req.AgentID).Update("gid", req.Gid)
	c.JSON(http.StatusOK, gin.H{"code": 0})
}

func (s *APIServer) RemoveGroupAgent(c *gin.Context) {
	var req AddGroupAgentReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000009, "message": err.Error()})
		return
	}

	s.db.Model(&model.AgentInfo{}).Where("id = ? AND gid = ?", req.AgentID, req.Gid).Update("gid", 0)
	c.JSON(http.StatusOK, gin.H{"code": 0})
}

// ==================== Schedule ====================

func (s *APIServer) CreateScheduleTask(c *gin.Context) {
	var req CreateScheduleReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000010, "message": err.Error()})
		return
	}

	uid := c.GetString("uid")
	tid := genTID()

	// Store the full task creation params as JSON for cron dispatch
	taskParams := CreateTaskReq{
		Name:         req.Name,
		Type:         model.TaskTypeContinuous,
		ProfilerType: req.ProfilerType,
		TargetIP:     req.TargetIP,
		PID:          req.PID,
		Duration:     0, // continuous: no duration limit
		Hz:           req.Hz,
	}
	paramsJSON, _ := json.Marshal(taskParams)

	mt := model.MultiTasks{
		TID:            tid,
		SubTIDs:        "[]",
		Type:           req.Type,
		Status:         model.TaskStatusPending,
		AnalysisStatus: model.AnalysisStatusPending,
		TriggerType:    model.TriggerCron,
		CronExpr:       req.CronExpr,
		ScheduleParams: string(paramsJSON),
		Enabled:        true,
	}
	if err := s.db.Create(&mt).Error; err != nil {
		s.logger.Error("create schedule task", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000010, "message": "create schedule task failed"})
		return
	}

	// Register with the cron scheduler
	if s.cronScheduler != nil {
		if err := s.cronScheduler.AddSchedule(tid, req.CronExpr); err != nil {
			s.logger.Error("register cron schedule", zap.String("tid", tid), zap.Error(err))
		}
	}

	s.logger.Info("schedule task created and registered",
		zap.String("tid", tid),
		zap.String("uid", uid),
		zap.String("cron", req.CronExpr),
	)

	c.JSON(http.StatusOK, gin.H{
		"code": 0,
		"data": gin.H{
			"tid":       tid,
			"cron_expr": req.CronExpr,
		},
	})
}

// ==================== Continuous Profile Segments ====================

// ==================== Continuous Profile Segments ====================

func (s *APIServer) ListSchedules(c *gin.Context) {
	var mts []model.MultiTasks
	s.db.Where("trigger_type = ?", model.TriggerCron).Order("created_at DESC").Find(&mts)

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": mts})
}

func (s *APIServer) ToggleSchedule(c *gin.Context) {
	tid := c.Param("tid")
	var req struct {
		Enabled bool `json:"enabled"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000013, "message": err.Error()})
		return
	}

	result := s.db.Model(&model.MultiTasks{}).Where("tid = ? AND trigger_type = ?", tid, model.TriggerCron).Update("enabled", req.Enabled)
	if result.RowsAffected == 0 {
		c.JSON(http.StatusNotFound, gin.H{"code": 4040013, "message": "schedule not found"})
		return
	}

	// Update in-memory cron
	if s.cronScheduler != nil {
		if req.Enabled {
			var mt model.MultiTasks
			s.db.Where("tid = ?", tid).First(&mt)
			if mt.CronExpr != "" {
				s.cronScheduler.AddSchedule(tid, mt.CronExpr)
			}
		} else {
			s.cronScheduler.RemoveSchedule(tid)
		}
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"tid": tid, "enabled": req.Enabled}})
}

func (s *APIServer) DeleteSchedule(c *gin.Context) {
	tid := c.Param("tid")

	result := s.db.Where("tid = ? AND trigger_type = ?", tid, model.TriggerCron).Delete(&model.MultiTasks{})
	if result.RowsAffected == 0 {
		c.JSON(http.StatusNotFound, gin.H{"code": 4040014, "message": "schedule not found"})
		return
	}

	// Remove from in-memory cron
	if s.cronScheduler != nil {
		s.cronScheduler.RemoveSchedule(tid)
	}

	s.logger.Info("schedule deleted", zap.String("tid", tid))
	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"tid": tid}})
}

func (s *APIServer) CreateSegment(c *gin.Context) {
	tid := c.Param("tid")
	var req CreateSegmentReq
	if err := c.ShouldBindJSON(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000011, "message": err.Error()})
		return
	}

	seg := model.ContinuousProfileSegment{
		TID:     tid,
		StartTs: req.StartTs,
		EndTs:   req.EndTs,
		S3Key:   req.S3Key,
	}
	if err := s.db.Create(&seg).Error; err != nil {
		s.logger.Error("create segment", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000011, "message": "create segment failed"})
		return
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"id": seg.ID}})
}

func (s *APIServer) ListSegments(c *gin.Context) {
	tid := c.Param("tid")

	var segments []model.ContinuousProfileSegment
	s.db.Where("tid = ?", tid).Order("start_ts ASC").Find(&segments)

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": segments})
}

// ==================== Analysis Suggestions ====================

func (s *APIServer) ListSuggestions(c *gin.Context) {
	tid := c.Param("tid")

	var suggestions []model.AnalysisSuggestion
	s.db.Where("tid = ?", tid).Order("status ASC, id ASC").Find(&suggestions)

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": suggestions})
}

// ==================== Profile Window Merge ====================

func (s *APIServer) GetProfileWindow(c *gin.Context) {
	tid := c.Param("tid")
	var req ProfileWindowReq
	if err := c.ShouldBindQuery(&req); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000012, "message": "start and end query params required"})
		return
	}
	if req.Start == 0 || req.End == 0 {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000012, "message": "start and end query params required"})
		return
	}

	// Find overlapping segments
	var segments []model.ContinuousProfileSegment
	s.db.Where("tid = ? AND start_ts < ? AND end_ts > ?", tid, req.End, req.Start).
		Order("start_ts ASC").Find(&segments)

	if len(segments) == 0 {
		c.JSON(http.StatusNotFound, gin.H{"code": 4040012, "message": "no segments in the requested window"})
		return
	}

	if s.storage == nil {
		c.JSON(http.StatusServiceUnavailable, gin.H{"code": 5030012, "message": "storage not configured"})
		return
	}

	// Merge collapsed stacks from all segments
	ctx := context.Background()
	merged := make(map[string]uint64)

	for _, seg := range segments {
		reader, err := s.storage.Get(ctx, seg.S3Key)
		if err != nil {
			s.logger.Warn("skip segment, cannot download", zap.String("s3_key", seg.S3Key), zap.Error(err))
			continue
		}
		// Parse collapsed stacks line by line
		scanner := bufio.NewScanner(reader)
		for scanner.Scan() {
			line := scanner.Text()
			if line == "" {
				continue
			}
			lastSpace := strings.LastIndex(line, " ")
			if lastSpace < 0 {
				continue
			}
			stack := line[:lastSpace]
			countStr := line[lastSpace+1:]
			count, err := strconv.ParseUint(countStr, 10, 64)
			if err != nil {
				continue
			}
			merged[stack] += count
		}
		reader.Close()
	}

	if len(merged) == 0 {
		c.JSON(http.StatusNotFound, gin.H{"code": 4040013, "message": "no data in the requested window"})
		return
	}

	// Build merged output
	var buf strings.Builder
	for stack, count := range merged {
		buf.WriteString(stack)
		buf.WriteByte(' ')
		buf.WriteString(strconv.FormatUint(count, 10))
		buf.WriteByte('\n')
	}

	c.Header("Content-Type", "text/plain; charset=utf-8")
	c.String(http.StatusOK, buf.String())
}

// ==================== Helpers ====================

func genTID() string {
	const charset = "abcdefghijklmnopqrstuvwxyz0123456789"
	b := make([]byte, 12)
	for i := range b {
		b[i] = charset[rand.Intn(len(charset))]
	}
	return string(b)
}

// dispatchTask sends the task to drop_server via gRPC ControlService.CreateTask.
// On failure it marks the task as FAILED with the error reason.
func (s *APIServer) dispatchTask(tid string, req CreateTaskReq) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	client := pb.NewControlClient(s.grpcCC)

	pbReq := &pb.CreateTaskRequest{
		TargetIp: req.TargetIP,
		Service:  "hotmethod",
		TaskDesc: &pb.TaskDesc{
			TaskId:       tid,
			TaskType:     uint32(req.Type),
			ProfilerType: uint32(req.ProfilerType),
			SampleArgv: &pb.RecordArgv{
				Hz:       uint32(req.Hz),
				Duration: uint64(req.Duration),
				Pid:      int32(req.PID),
				Event:    req.Event,
			},
		},
	}

	resp, err := client.CreateTask(ctx, pbReq)
	if err != nil {
		reason := fmt.Sprintf("gRPC ControlService.CreateTask failed: %v", err)
		s.logger.Error("dispatch task failed", zap.String("tid", tid), zap.Error(err))
		if uerr := s.updateTaskStatus(context.Background(), tid, model.TaskStatusFailed, reason); uerr != nil {
			s.logger.Error("mark task failed on dispatch", zap.Error(uerr))
		}
		return
	}

	if !resp.Success {
		reason := fmt.Sprintf("drop_server rejected task: %s", resp.Message)
		s.logger.Warn("task rejected by drop_server", zap.String("tid", tid), zap.String("reason", reason))
		if uerr := s.updateTaskStatus(context.Background(), tid, model.TaskStatusFailed, reason); uerr != nil {
			s.logger.Error("mark task failed on rejection", zap.Error(uerr))
		}
		return
	}

	s.logger.Info("task dispatched to drop_server", zap.String("tid", tid), zap.String("target_ip", req.TargetIP))
}

// ==================== File Upload ====================

// UploadTaskFile accepts a multipart file upload for a task and stores it in MinIO.
// The cos_key format is: <tid>/<filename>
func (s *APIServer) UploadTaskFile(c *gin.Context) {
	tid := c.Param("tid")
	filename := c.Param("filename")

	if tid == "" || filename == "" {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000100, "message": "tid and filename required"})
		return
	}

	// Strip leading slash from filename if present
	filename = strings.TrimPrefix(filename, "/")

	if s.storage == nil {
		c.JSON(http.StatusServiceUnavailable, gin.H{"code": 5030100, "message": "storage not configured"})
		return
	}

	file, header, err := c.Request.FormFile("file")
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000101, "message": "file upload failed: " + err.Error()})
		return
	}
	defer file.Close()

	cosKey := tid + "/" + filename
	ctx := context.Background()

	// Detect content type from extension
	contentType := "application/octet-stream"
	if strings.HasSuffix(filename, ".svg") {
		contentType = "image/svg+xml"
	} else if strings.HasSuffix(filename, ".json") {
		contentType = "application/json"
	} else if strings.HasSuffix(filename, ".txt") {
		contentType = "text/plain"
	}

	if err := s.storage.Put(ctx, cosKey, file, header.Size, contentType); err != nil {
		s.logger.Error("upload to minio failed", zap.String("key", cosKey), zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000100, "message": "storage upload failed"})
		return
	}

	// Update cos_key on the task if not set
	var task model.HotmethodTask
	if err := s.db.Where("tid = ?", tid).First(&task).Error; err == nil {
		if task.CosKey == "" {
			s.db.Model(&task).Update("cos_key", cosKey)
		}
	}

	s.logger.Info("file uploaded", zap.String("tid", tid), zap.String("key", cosKey), zap.Int64("size", header.Size))
	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"cos_key": cosKey}})
}
