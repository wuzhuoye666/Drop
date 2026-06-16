package model

import (
	"time"

	"gorm.io/gorm"
)

// BaseModel provides common fields for all models.
type BaseModel struct {
	ID        uint           `gorm:"primaryKey" json:"id"`
	CreatedAt time.Time      `json:"created_at"`
	UpdatedAt time.Time      `json:"updated_at"`
	DeletedAt gorm.DeletedAt `gorm:"index" json:"deleted_at,omitempty"`
}

// ==================== Business Tables ====================

// UserInfo 用户信息
type UserInfo struct {
	BaseModel
	UID    string `gorm:"uniqueIndex;size:64;not null" json:"uid"`
	Name   string `gorm:"size:128;not null" json:"name"`
	Groups string `gorm:"type:jsonb;column:groups;default:'[]'" json:"groups"` // JSON array of group IDs
	Key    string `gorm:"size:256" json:"key,omitempty"`
}

func (UserInfo) TableName() string { return "user_info" }

// AgentInfo Agent 注册信息
type AgentInfo struct {
	BaseModel
	Hostname    string `gorm:"size:256;not null" json:"hostname"`
	IPAddr      string `gorm:"size:64;not null;uniqueIndex" json:"ip_addr"`
	Online      bool   `gorm:"default:false;index:idx_ip_online,priority:2" json:"online"`
	UID         string `gorm:"size:64;index" json:"uid"`
	Gid         uint   `json:"gid"`
	Version     string `gorm:"size:64" json:"version"`
	Environment string `gorm:"size:512" json:"environment"`
	LastHB      time.Time
}

func (AgentInfo) TableName() string { return "agent_info" }

// HotmethodTask 任务核心表
type HotmethodTask struct {
	BaseModel
	TID            string         `gorm:"column:tid;uniqueIndex;size:64;not null" json:"tid"`
	Name           string         `gorm:"size:256;not null" json:"name"`
	Type           int            `gorm:"not null;default:0" json:"type"`            // 0=单次 1=连续 2=多目标
	ProfilerType   int            `gorm:"not null;default:0" json:"profiler_type"`   // 0=perf 1=async-profiler 2=pprof 3=ebpf
	TargetIP       string         `gorm:"size:64;not null;index:idx_ip_status,priority:1" json:"target_ip"`
	RequestParams  string         `gorm:"type:jsonb;default:'{}'" json:"request_params"` // 完整请求参数
	Status         int            `gorm:"not null;default:0;index:idx_ip_status,priority:2" json:"status"`           // 0=PENDING 1=RUNNING 2=UPLOADING 3=DONE 4=FAILED
	StatusInfo     string         `gorm:"type:text;not null;default:''" json:"status_info"`   // 每次状态迁移必填reason
	AnalysisStatus int            `gorm:"not null;default:0" json:"analysis_status"` // 0=待分析 1=分析中 2=成功 3=失败
	UID            string         `gorm:"size:64;index" json:"uid"`
	UserName       string         `gorm:"size:128" json:"user_name"`
	CreateTime     time.Time      `json:"create_time"`
	BeginTime      *time.Time     `json:"begin_time,omitempty"`
	EndTime        *time.Time     `json:"end_time,omitempty"`
	MasterTaskTid  string         `gorm:"size:64" json:"master_task_tid,omitempty"`
}

func (HotmethodTask) TableName() string { return "hotmethod_task" }

// MultiTasks 多目标任务
type MultiTasks struct {
	TID        string `gorm:"primaryKey;size:64" json:"tid"`
	SubTIDs    string `gorm:"type:jsonb;default:'[]'" json:"sub_tids"` // JSON array of sub task TIDs
	Type       int    `gorm:"not null;default:0" json:"type"`
	Status     int    `gorm:"not null;default:0" json:"status"`
	AnalysisStatus int `gorm:"not null;default:0" json:"analysis_status"`
	TriggerType int    `gorm:"not null;default:0" json:"trigger_type"` // 0=手动 1=定时
}

func (MultiTasks) TableName() string { return "multi_tasks" }

// Group 用户组
type Group struct {
	Gid     uint   `gorm:"primaryKey" json:"gid"`
	Name    string `gorm:"size:128;not null;uniqueIndex" json:"name"`
	OwnerID string `gorm:"size:64;not null;index" json:"owner_id"`
}

func (Group) TableName() string { return "groups" }

// GroupMember 组成员
type GroupMember struct {
	Gid uint   `gorm:"primaryKey" json:"gid"`
	UID string `gorm:"primaryKey;size:64" json:"uid"`
}

func (GroupMember) TableName() string { return "group_members" }

// AnalysisSuggestion 分析建议
type AnalysisSuggestion struct {
	BaseModel
	TID          string `gorm:"size:64;not null;index" json:"tid"`
	Func         string `gorm:"size:512;not null" json:"func"`
	Suggestion   string `gorm:"type:text;not null" json:"suggestion"`
	AISuggestion string `gorm:"type:text" json:"ai_suggestion,omitempty"`
	Status       int    `gorm:"not null;default:0" json:"status"` // 0=规则 1=AI
}

func (AnalysisSuggestion) TableName() string { return "analysis_suggestion" }

// ==================== Audit Tables ====================

// TaskStatusHistory 任务状态迁移审计
type TaskStatusHistory struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	TID       string    `gorm:"column:tid;size:64;not null;index" json:"tid"`
	OldStatus int       `gorm:"not null" json:"old_status"`
	NewStatus int       `gorm:"not null" json:"new_status"`
	Reason    string    `gorm:"type:text;not null" json:"reason"`
	Timestamp time.Time `gorm:"not null;index" json:"timestamp"`
}

func (TaskStatusHistory) TableName() string { return "task_status_history" }

// AgentAuditLog Agent 状态变更审计
type AgentAuditLog struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	AgentID   uint      `gorm:"not null;index" json:"agent_id"`
	EventType string    `gorm:"size:64;not null;index" json:"event_type"` // online, offline
	Reason    string    `gorm:"type:text;not null" json:"reason"`
	Timestamp time.Time `gorm:"not null;index" json:"timestamp"`
}

func (AgentAuditLog) TableName() string { return "agent_audit_log" }

// ==================== Constants ====================

// Task status
const (
	TaskStatusPending   = 0
	TaskStatusRunning   = 1
	TaskStatusUploading = 2
	TaskStatusDone      = 3
	TaskStatusFailed    = 4
)

// Analysis status
const (
	AnalysisStatusPending   = 0
	AnalysisStatusRunning   = 1
	AnalysisStatusSuccess   = 2
	AnalysisStatusFailed    = 3
)

// Task type
const (
	TaskTypeSingle     = 0
	TaskTypeContinuous = 1
	TaskTypeMulti      = 2
)

// Profiler type
const (
	ProfilerPerf          = 0
	ProfilerAsyncProfiler = 1
	ProfilerPprof         = 2
	ProfilerEbpf          = 3
)

// Trigger type
const (
	TriggerManual = 0
	TriggerCron   = 1
)

// Now returns current UTC time.
func Now() time.Time {
	return time.Now().UTC()
}
