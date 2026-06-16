package server

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/drop/apiserver/config"
	"github.com/drop/apiserver/middleware"
	"github.com/drop/apiserver/model"
	"github.com/gin-gonic/gin"
	"github.com/stretchr/testify/assert"
	"go.uber.org/zap"
	"gorm.io/driver/postgres"
	"gorm.io/gorm"
	gormlogger "gorm.io/gorm/logger"
)

func setupTestServer(t *testing.T) (*APIServer, *gin.Engine) {
	t.Helper()
	gin.SetMode(gin.TestMode)
	logger, _ := zap.NewDevelopment()

	dsn := "host=127.0.0.1 port=5432 user=drop password=drop dbname=drop sslmode=disable"
	db, err := gorm.Open(postgres.Open(dsn), &gorm.Config{Logger: gormlogger.Default.LogMode(gormlogger.Silent)})
	assert.NoError(t, err, "connect DB")

	err = db.AutoMigrate(
		&model.UserInfo{},
		&model.AgentInfo{},
		&model.HotmethodTask{},
		&model.MultiTasks{},
		&model.Group{},
		&model.GroupMember{},
		&model.AnalysisSuggestion{},
		&model.TaskStatusHistory{},
		&model.AgentAuditLog{},
	)
	assert.NoError(t, err, "auto migrate")

	cfg := &config.Config{
		Server: config.ServerConfig{DevMode: true, Port: ":0"},
	}

	srv := New(db, nil, nil, cfg, logger)

	r := gin.New()
	r.Use(gin.Recovery())
	r.Use(middleware.Auth(true, logger))

	r.POST("/api/v1/tasks", srv.CreateTask)
	r.GET("/api/v1/tasks", srv.ListTasks)
	r.GET("/api/v1/tasks/:tid", srv.GetTask)
	r.DELETE("/api/v1/tasks/:tid", srv.DeleteTask)
	r.POST("/api/v1/tasks/:tid/retry", srv.RetryTask)
	r.GET("/api/v1/agents", srv.ListAgents)
	r.GET("/users", srv.GetUser)

	return srv, r
}

func TestCreateAndGetTask(t *testing.T) {
	_, r := setupTestServer(t)

	// Create
	body, _ := json.Marshal(CreateTaskReq{
		Name: "unit-test", Type: 0, ProfilerType: 0,
		TargetIP: "10.0.0.1", PID: 1234, Duration: 5, Hz: 99,
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
	assert.NotEmpty(t, tid, "should return a tid")

	// Get
	w = httptest.NewRecorder()
	req, _ = http.NewRequest("GET", "/api/v1/tasks/"+tid, nil)
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusOK, w.Code)
	json.Unmarshal(w.Body.Bytes(), &resp)
	taskData := resp["data"].(map[string]interface{})["task"].(map[string]interface{})
	assert.Equal(t, "unit-test", taskData["name"])
}

func TestListTasks(t *testing.T) {
	_, r := setupTestServer(t)

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/api/v1/tasks?page=1&page_size=10", nil)
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusOK, w.Code)

	var resp map[string]interface{}
	json.Unmarshal(w.Body.Bytes(), &resp)
	assert.Equal(t, float64(0), resp["code"])
}

func TestDeleteNonexistentTask(t *testing.T) {
	_, r := setupTestServer(t)

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("DELETE", "/api/v1/tasks/nonexistent", nil)
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusNotFound, w.Code)
}

func TestRetryOnlyFailedTasks(t *testing.T) {
	_, r := setupTestServer(t)

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("POST", "/api/v1/tasks/never-exist/retry", nil)
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusBadRequest, w.Code)
}
