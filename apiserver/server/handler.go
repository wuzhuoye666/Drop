package server

import (
	"context"
	"encoding/json"
	"math/rand"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/drop/apiserver/model"
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
		// Auto-create user on first visit
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
		OldStatus: -1, // -1 means created from nothing
		NewStatus: model.TaskStatusPending,
		Reason:    "任务已创建",
		Timestamp: task.CreateTime,
	}
	s.db.Create(&history)

	// Dispatch via gRPC to drop_server
	if s.grpcCC != nil {
		go s.dispatchTask(tid, req)
	} else {
		// gRPC unavailable — mark task as FAILED immediately rather than faking RUNNING
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

	// Generate presigned URLs for task outputs if storage available
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

	// Allow delete if user owns the task OR is in a group that shares the task owner's tasks
	result := s.db.Where("tid = ? AND (uid = ? OR uid IN (SELECT uid FROM group_members WHERE gid IN (SELECT gid FROM group_members WHERE uid = ?)))", tid, uid, uid).Delete(&model.HotmethodTask{})
	if result.RowsAffected == 0 {
		c.JSON(http.StatusNotFound, gin.H{"code": 4040002, "message": "task not found or no permission"})
		return
	}

	// Also soft-delete related suggestions
	s.db.Where("tid = ?", tid).Delete(&model.AnalysisSuggestion{})

	// Clean up COS files
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

	// Transition back to PENDING
	if err := s.updateTaskStatus(context.Background(), tid, model.TaskStatusPending, "用户触发重试"); err != nil {
		s.logger.Error("retry task status update", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000003, "message": err.Error()})
		return
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
	if ip == "" {
		c.JSON(http.StatusBadRequest, gin.H{"code": 4000003, "message": "target_ip required"})
		return
	}

	// In production, this proxies to drop_server via gRPC StatAgent
	// For now return agent_info from PG
	var agent model.AgentInfo
	if err := s.db.Where("ip_addr = ?", ip).First(&agent).Error; err != nil {
		c.JSON(http.StatusNotFound, gin.H{"code": 4040003, "message": "agent not found"})
		return
	}

	c.JSON(http.StatusOK, gin.H{"code": 0, "data": agent})
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

	// Add owner as member
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

	// Persist to multi_tasks as a scheduled task
	mt := model.MultiTasks{
		TID:         tid,
		SubTIDs:     "[]",
		Type:        req.Type,
		Status:      model.TaskStatusPending,
		AnalysisStatus: model.AnalysisStatusPending,
		TriggerType: model.TriggerCron,
	}
	if err := s.db.Create(&mt).Error; err != nil {
		s.logger.Error("create schedule task", zap.Error(err))
		c.JSON(http.StatusInternalServerError, gin.H{"code": 5000010, "message": "create schedule task failed"})
		return
	}

	// Cron dispatch is deferred to Phase 7
	s.logger.Info("schedule task created (cron dispatch pending Phase 7)",
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
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	// TODO: call pb.NewControlServiceClient(s.grpcCC).CreateTask(ctx, req) when proto is wired
	// For now, report failure since the proto-generated stub is not yet integrated
	_ = ctx
	reason := "gRPC ControlService client not yet integrated — dispatch stub"
	s.logger.Warn("dispatch task stub called", zap.String("tid", tid))
	if err := s.updateTaskStatus(context.Background(), tid, model.TaskStatusFailed, reason); err != nil {
		s.logger.Error("mark task failed on dispatch", zap.Error(err))
	}
}
