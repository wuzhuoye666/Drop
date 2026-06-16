package server

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/drop/apiserver/model"
	"github.com/gin-gonic/gin"
	"gorm.io/driver/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

func setupTestDB(t *testing.T) *gorm.DB {
	t.Helper()
	db, err := gorm.Open(sqlite.Open(":memory:"), &gorm.Config{Logger: logger.Default.LogMode(logger.Silent)})
	if err != nil {
		t.Fatalf("open sqlite: %v", err)
	}
	db.AutoMigrate(
		&model.HotmethodTask{},
		&model.MultiTasks{},
		&model.ContinuousProfileSegment{},
		&model.TaskStatusHistory{},
		&model.AgentInfo{},
	)
	return db
}

func TestCreateSegment(t *testing.T) {
	db := setupTestDB(t)
	srv := &APIServer{db: db}

	// Create a continuous task first
	task := model.HotmethodTask{
		TID:        "segtest01",
		Name:       "test",
		Type:       model.TaskTypeContinuous,
		TargetIP:   "10.0.0.1",
		Status:     model.TaskStatusRunning,
		StatusInfo:  "running",
		CreateTime: model.Now(),
	}
	db.Create(&task)

	gin.SetMode(gin.TestMode)
	r := gin.New()
	r.POST("/api/v1/tasks/:tid/segments", srv.CreateSegment)

	body := `{"start_ts":1718500000,"end_ts":1718500300,"s3_key":"segtest01/1718500000/collapsed.txt"}`
	req := httptest.NewRequest(http.MethodPost, "/api/v1/tasks/segtest01/segments", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String())
	}

	var count int64
	db.Model(&model.ContinuousProfileSegment{}).Where("tid = ?", "segtest01").Count(&count)
	if count != 1 {
		t.Fatalf("expected 1 segment, got %d", count)
	}
}

func TestListSegments(t *testing.T) {
	db := setupTestDB(t)
	srv := &APIServer{db: db}

	// Create segments
	for i := 0; i < 3; i++ {
		db.Create(&model.ContinuousProfileSegment{
			TID:     "listest01",
			StartTs: int64(1718500000 + i*300),
			EndTs:   int64(1718500300 + i*300),
			S3Key:   "listest01/segment.txt",
		})
	}

	gin.SetMode(gin.TestMode)
	r := gin.New()
	r.GET("/api/v1/tasks/:tid/segments", srv.ListSegments)

	req := httptest.NewRequest(http.MethodGet, "/api/v1/tasks/listest01/segments", nil)
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String())
	}

	var resp map[string]interface{}
	json.Unmarshal(w.Body.Bytes(), &resp)
	data := resp["data"].([]interface{})
	if len(data) != 3 {
		t.Fatalf("expected 3 segments, got %d", len(data))
	}
}

func TestGetProfileWindow_NoData(t *testing.T) {
	db := setupTestDB(t)
	srv := &APIServer{db: db}

	gin.SetMode(gin.TestMode)
	r := gin.New()
	r.GET("/api/v1/tasks/:tid/profile-window", srv.GetProfileWindow)

	req := httptest.NewRequest(http.MethodGet, "/api/v1/tasks/nonexist/profile-window?start=1718500000&end=1718500300", nil)
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)

	if w.Code != http.StatusNotFound {
		t.Fatalf("expected 404 for no segments, got %d", w.Code)
	}
}

func TestToggleSchedule(t *testing.T) {
	db := setupTestDB(t)
	srv := &APIServer{db: db}

	// Create a schedule
	db.Create(&model.MultiTasks{
		TID:         "schtoggle01",
		TriggerType: model.TriggerCron,
		CronExpr:    "0 */5 * * * *",
		Enabled:     true,
	})

	gin.SetMode(gin.TestMode)
	r := gin.New()
	api := r.Group("/api/v1")
	api.PUT("/schedule/:tid/toggle", srv.ToggleSchedule)

	// Pause
	body := `{"enabled":false}`
	req := httptest.NewRequest(http.MethodPut, "/api/v1/schedule/schtoggle01/toggle", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String())
	}

	var mt model.MultiTasks
	db.Where("tid = ?", "schtoggle01").First(&mt)
	if mt.Enabled {
		t.Fatal("expected schedule to be paused")
	}
}
