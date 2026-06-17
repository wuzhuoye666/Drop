package server

import (
	"testing"

	"github.com/drop/apiserver/config"
	"github.com/drop/apiserver/model"
	"go.uber.org/zap"
	"gorm.io/driver/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"github.com/gin-gonic/gin"
)

// setupTestServer creates an APIServer with an in-memory SQLite DB and a Gin router for testing.
func setupTestServer(t *testing.T) (*APIServer, *gin.Engine) {
	t.Helper()

	gin.SetMode(gin.TestMode)

	db, err := gorm.Open(sqlite.Open(":memory:"), &gorm.Config{
		Logger: logger.Default.LogMode(logger.Silent),
	})
	if err != nil {
		t.Fatalf("open sqlite: %v", err)
	}

	// AutoMigrate all models
	db.AutoMigrate(
		&model.UserInfo{},
		&model.AgentInfo{},
		&model.HotmethodTask{},
		&model.MultiTasks{},
		&model.Group{},
		&model.GroupMember{},
		&model.AnalysisSuggestion{},
		&model.TaskStatusHistory{},
		&model.AgentAuditLog{},
		&model.ContinuousProfileSegment{},
	)

	log := zap.NewNop()
	cfg := &config.Config{
		Server: config.ServerConfig{DevMode: true, Port: ":0"},
	}

	srv := New(db, nil, nil, cfg, log)

	r := gin.New()
	r.Use(gin.Recovery())

	// Auth middleware (dev mode — injects test user)
	api := r.Group("/api/v1")
	api.Use(func(c *gin.Context) {
		c.Set("uid", "test-uid")
		c.Set("user_name", "tester")
		c.Next()
	})
	{
		api.POST("/tasks", srv.CreateTask)
		api.GET("/tasks", srv.ListTasks)
		api.GET("/tasks/:tid", srv.GetTask)
		api.DELETE("/tasks/:tid", srv.DeleteTask)
		api.POST("/tasks/:tid/retry", srv.RetryTask)
		api.GET("/tasks/:tid/suggestions", srv.ListSuggestions)
		api.GET("/agents", srv.ListAgents)
		api.POST("/nl/chat", srv.NLChat)
	}

	// Also register internal upload route (no auth)
	r.POST("/api/v1/tasks/:tid/upload/*filename", srv.UploadTaskFile)

	return srv, r
}
