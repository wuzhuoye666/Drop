package server

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestCreateEbpfTask(t *testing.T) {
	_, r := setupTestServer(t)

	// Create an eBPF off-CPU task
	body, _ := json.Marshal(CreateTaskReq{
		Name:         "ebpf-offcpu-test",
		Type:         0,
		ProfilerType: 3, // eBPF
		TargetIP:     "10.0.0.1",
		PID:          0, // system-wide
		Duration:     5,
		Hz:           0, // not relevant for eBPF
	})

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/api/v1/tasks", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	assert.Equal(t, http.StatusOK, w.Code)

	var resp map[string]interface{}
	json.Unmarshal(w.Body.Bytes(), &resp)
	data := resp["data"].(map[string]interface{})
	tid := data["tid"].(string)
	assert.NotEmpty(t, tid, "should return a tid for eBPF task")

	// Verify the task via GET
	w = httptest.NewRecorder()
	req, _ = http.NewRequest("GET", "/api/v1/tasks/"+tid, nil)
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusOK, w.Code)

	json.Unmarshal(w.Body.Bytes(), &resp)
	taskData := resp["data"].(map[string]interface{})["task"].(map[string]interface{})
	assert.Equal(t, "ebpf-offcpu-test", taskData["name"])
	assert.Equal(t, float64(3), taskData["profiler_type"], "profiler_type should be 3 (eBPF)")
}

func TestListTasksIncludesEbpf(t *testing.T) {
	_, r := setupTestServer(t)

	// Create an eBPF task
	body, _ := json.Marshal(CreateTaskReq{
		Name:         "ebpf-list-test",
		Type:         0,
		ProfilerType: 3,
		TargetIP:     "10.0.0.2",
		PID:          1234,
		Duration:     10,
		Hz:           0,
	})
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/api/v1/tasks", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusOK, w.Code)

	// List tasks — should include the eBPF task
	w = httptest.NewRecorder()
	req, _ = http.NewRequest("GET", "/api/v1/tasks?page=1&page_size=50", nil)
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusOK, w.Code)

	var resp map[string]interface{}
	json.Unmarshal(w.Body.Bytes(), &resp)
	listData := resp["data"].(map[string]interface{})
	tasksRaw, ok := listData["list"].([]interface{})
	assert.True(t, ok, "response should have list array")
	assert.NotEmpty(t, tasksRaw, "task list should not be empty")

	// Find our eBPF task
	found := false
	for _, tRaw := range tasksRaw {
		task := tRaw.(map[string]interface{})
		if task["name"] == "ebpf-list-test" {
			found = true
			assert.Equal(t, float64(3), task["profiler_type"])
			break
		}
	}
	assert.True(t, found, "eBPF task should appear in task list")
}
