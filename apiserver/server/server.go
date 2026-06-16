package server

import (
	"context"
	"fmt"

	"github.com/drop/apiserver/config"
	"github.com/drop/apiserver/model"
	"github.com/drop/apiserver/pkg/storage/minio"
	"go.uber.org/zap"
	"google.golang.org/grpc"
	"gorm.io/gorm"
)

// APIServer holds all shared dependencies.
type APIServer struct {
	db      *gorm.DB
	grpcCC  *grpc.ClientConn
	storage *minio.Storage
	cfg     *config.Config
	logger  *zap.Logger
}

// New creates an APIServer.
func New(db *gorm.DB, cc *grpc.ClientConn, storage *minio.Storage, cfg *config.Config, logger *zap.Logger) *APIServer {
	return &APIServer{
		db:      db,
		grpcCC:  cc,
		storage: storage,
		cfg:     cfg,
		logger:  logger,
	}
}

// updateTaskStatus performs a validated status transition inside a single transaction.
// It writes to hotmethod_task and inserts a task_status_history row.
func (s *APIServer) updateTaskStatus(ctx context.Context, tid string, newStatus int, reason string) error {
	if reason == "" {
		reason = "no reason provided"
	}

	return s.db.WithContext(ctx).Transaction(func(tx *gorm.DB) error {
		var task model.HotmethodTask
		if err := tx.Set("gorm:query_option", "FOR UPDATE").
			Where("tid = ?", tid).First(&task).Error; err != nil {
			return fmt.Errorf("find task %s: %w", tid, err)
		}

		if !isTransitionAllowed(task.Status, newStatus) {
			s.logger.Warn("illegal status transition",
				zap.String("tid", tid),
				zap.Int("old", task.Status),
				zap.Int("new", newStatus),
			)
			return fmt.Errorf("illegal transition %d→%d for task %s", task.Status, newStatus, tid)
		}

		oldStatus := task.Status // capture before GORM Updates mutates the struct

		now := model.Now()
		updates := map[string]interface{}{
			"status":      newStatus,
			"status_info": reason,
		}
		switch newStatus {
		case model.TaskStatusRunning:
			updates["begin_time"] = now
		case model.TaskStatusDone, model.TaskStatusFailed:
			updates["end_time"] = now
		}

		if err := tx.Model(&task).Updates(updates).Error; err != nil {
			return fmt.Errorf("update task: %w", err)
		}

		history := model.TaskStatusHistory{
			TID:       tid,
			OldStatus: oldStatus,
			NewStatus: newStatus,
			Reason:    reason,
			Timestamp: now,
		}
		if err := tx.Create(&history).Error; err != nil {
			return fmt.Errorf("insert history: %w", err)
		}

		return nil
	})
}

// isTransitionAllowed enforces the state machine:
//
//	PENDING(0) → RUNNING(1)
//	RUNNING(1) → UPLOADING(2)
//	UPLOADING(2) → DONE(3)
//	any → FAILED(4)
//	FAILED(4) → PENDING(0)  (retry)
func isTransitionAllowed(old, new int) bool {
	if old == new {
		return false
	}
	switch old {
	case model.TaskStatusPending:
		return new == model.TaskStatusRunning || new == model.TaskStatusFailed
	case model.TaskStatusRunning:
		return new == model.TaskStatusUploading || new == model.TaskStatusFailed
	case model.TaskStatusUploading:
		return new == model.TaskStatusDone || new == model.TaskStatusFailed
	case model.TaskStatusFailed:
		return new == model.TaskStatusPending // retry
	case model.TaskStatusDone:
		return false
	}
	return false
}
