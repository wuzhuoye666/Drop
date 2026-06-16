package server

import (
	"testing"

	"github.com/drop/apiserver/model"
	"github.com/stretchr/testify/assert"
)

func TestIsTransitionAllowed(t *testing.T) {
	tests := []struct {
		name     string
		old      int
		new      int
		expected bool
	}{
		{"PENDING→RUNNING", model.TaskStatusPending, model.TaskStatusRunning, true},
		{"PENDING→FAILED", model.TaskStatusPending, model.TaskStatusFailed, true},
		{"PENDING→DONE", model.TaskStatusPending, model.TaskStatusDone, false},
		{"PENDING→UPLOADING", model.TaskStatusPending, model.TaskStatusUploading, false},
		{"RUNNING→UPLOADING", model.TaskStatusRunning, model.TaskStatusUploading, true},
		{"RUNNING→FAILED", model.TaskStatusRunning, model.TaskStatusFailed, true},
		{"RUNNING→DONE", model.TaskStatusRunning, model.TaskStatusDone, false},
		{"UPLOADING→DONE", model.TaskStatusUploading, model.TaskStatusDone, true},
		{"UPLOADING→FAILED", model.TaskStatusUploading, model.TaskStatusFailed, true},
		{"DONE→RUNNING", model.TaskStatusDone, model.TaskStatusRunning, false},
		{"DONE→FAILED", model.TaskStatusDone, model.TaskStatusFailed, false},
		{"FAILED→PENDING", model.TaskStatusFailed, model.TaskStatusPending, true},
		{"FAILED→RUNNING", model.TaskStatusFailed, model.TaskStatusRunning, false},
		{"same status", model.TaskStatusPending, model.TaskStatusPending, false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			assert.Equal(t, tt.expected, isTransitionAllowed(tt.old, tt.new))
		})
	}
}
