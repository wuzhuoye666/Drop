package server

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/drop/apiserver/model"
	"github.com/stretchr/testify/assert"
)

func TestNLChat_ParseCPUTask(t *testing.T) {
	srv, r := setupTestServer(t)

	body, _ := json.Marshal(NLChatReq{Message: "帮我看看CPU飙高 采集10秒"})
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/api/v1/nl/chat", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	assert.Equal(t, http.StatusOK, w.Code)

	var resp map[string]interface{}
	json.Unmarshal(w.Body.Bytes(), &resp)

	data, ok := resp["data"].(map[string]interface{})
	assert.True(t, ok, "response should have data object")
	assert.Equal(t, "task_created", data["type"])
	tid, _ := data["tid"].(string)
	assert.NotEmpty(t, tid, "should create a task")

	var count int64
	srv.db.Model(&model.HotmethodTask{}).Where("tid = ?", tid).Count(&count)
	assert.Equal(t, int64(1), count)
}

func TestNLChat_ParseEbpfTask(t *testing.T) {
	_, r := setupTestServer(t)

	body, _ := json.Marshal(NLChatReq{Message: "io阻塞严重 看看30秒"})
	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/api/v1/nl/chat", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	assert.Equal(t, http.StatusOK, w.Code)

	var resp map[string]interface{}
	json.Unmarshal(w.Body.Bytes(), &resp)

	data, ok := resp["data"].(map[string]interface{})
	assert.True(t, ok)
	assert.Equal(t, "task_created", data["type"])
}

func TestListSuggestions_Empty(t *testing.T) {
	_, r := setupTestServer(t)

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/api/v1/tasks/nonexistent/suggestions", nil)
	r.ServeHTTP(w, req)

	assert.Equal(t, http.StatusOK, w.Code)

	var resp map[string]interface{}
	json.Unmarshal(w.Body.Bytes(), &resp)
	data, ok := resp["data"].([]interface{})
	if ok {
		assert.Empty(t, data)
	}
	// data can be nil when no rows — that's acceptable
}

func TestListSuggestions_WithData(t *testing.T) {
	srv, r := setupTestServer(t)

	task := model.HotmethodTask{
		TID:           "sug-test-001",
		Name:          "suggestion test",
		ProfilerType:  0,
		TargetIP:      "127.0.0.1",
		Status:        model.TaskStatusDone,
		AnalysisStatus: model.AnalysisStatusSuccess,
		UID:           "test-uid",
		UserName:      "tester",
		CreateTime:    model.Now(),
	}
	srv.db.Create(&task)

	sug1 := model.AnalysisSuggestion{TID: "sug-test-001", Func: "malloc_internal", Suggestion: "Consider jemalloc", Status: 0}
	sug2 := model.AnalysisSuggestion{TID: "sug-test-001", Func: "__ai_summary__", AISuggestion: `{"diagnosis":"CPU bound","confidence":0.8}`, Status: 1}
	srv.db.Create(&sug1)
	srv.db.Create(&sug2)

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/api/v1/tasks/sug-test-001/suggestions", nil)
	r.ServeHTTP(w, req)

	assert.Equal(t, http.StatusOK, w.Code)

	var resp map[string]json.RawMessage
	json.Unmarshal(w.Body.Bytes(), &resp)

	var data []interface{}
	if raw, ok := resp["data"]; ok && string(raw) != "null" {
		json.Unmarshal(raw, &data)
	}
	assert.Len(t, data, 2)
}

func TestDeleteTask_Cascades(t *testing.T) {
	srv, r := setupTestServer(t)

	task := model.HotmethodTask{
		TID: "del-casc-001", Name: "cascade", ProfilerType: 0,
		TargetIP: "127.0.0.1", Status: model.TaskStatusDone,
		UID: "test-uid", UserName: "tester", CreateTime: model.Now(),
	}
	srv.db.Create(&task)
	srv.db.Create(&model.AnalysisSuggestion{TID: "del-casc-001", Func: "f", Suggestion: "s", Status: 0})

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("DELETE", "/api/v1/tasks/del-casc-001", nil)
	r.ServeHTTP(w, req)

	assert.Equal(t, http.StatusOK, w.Code)

	var count int64
	srv.db.Model(&model.AnalysisSuggestion{}).Where("tid = ?", "del-casc-001").Count(&count)
	assert.Equal(t, int64(0), count, "suggestions should be cascade-deleted")
}
