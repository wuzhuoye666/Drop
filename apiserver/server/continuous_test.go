package server

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/drop/apiserver/model"
	"github.com/drop/apiserver/pkg/storage"
	"github.com/gin-gonic/gin"
	"go.uber.org/zap"
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

func TestGetProfileWindow_Merge(t *testing.T) {
	db := setupTestDB(t)

	store := &mockMergeStorage{
		objects: map[string]string{
			"mergetest/1718500000/collapsed.txt": "main;foo 10\nmain;bar 5\n",
			"mergetest/1718500300/collapsed.txt": "main;foo 20\nmain;baz 8\n",
		},
	}
	srv := &APIServer{db: db, storage: store}

	db.Create(&model.ContinuousProfileSegment{
		TID: "mergetest", StartTs: 1718500000, EndTs: 1718500300,
		S3Key: "mergetest/1718500000/collapsed.txt",
	})
	db.Create(&model.ContinuousProfileSegment{
		TID: "mergetest", StartTs: 1718500300, EndTs: 1718500600,
		S3Key: "mergetest/1718500300/collapsed.txt",
	})

	gin.SetMode(gin.TestMode)
	r := gin.New()
	r.GET("/api/v1/tasks/:tid/profile-window", srv.GetProfileWindow)

	req := httptest.NewRequest(http.MethodGet, "/api/v1/tasks/mergetest/profile-window?start=1718500000&end=1718500600", nil)
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String())
	}

	body := w.Body.String()
	if !strings.Contains(body, "main;foo 30") {
		t.Errorf("expected merged 'main;foo 30', got: %s", body)
	}
	if !strings.Contains(body, "main;bar 5") {
		t.Errorf("expected 'main;bar 5', got: %s", body)
	}
	if !strings.Contains(body, "main;baz 8") {
		t.Errorf("expected 'main;baz 8', got: %s", body)
	}
}

// mockMergeStorage implements storage.Storage for testing
type mockMergeStorage struct {
	objects map[string]string
}

func (m *mockMergeStorage) Put(ctx context.Context, key string, reader io.Reader, size int64, contentType string) error {
	return nil
}

func (m *mockMergeStorage) Get(ctx context.Context, key string) (io.ReadCloser, error) {
	content, ok := m.objects[key]
	if !ok {
		return nil, fmt.Errorf("not found: %s", key)
	}
	return io.NopCloser(strings.NewReader(content)), nil
}

func (m *mockMergeStorage) PreSign(ctx context.Context, key string) (string, error) { return "", nil }
func (m *mockMergeStorage) Delete(ctx context.Context, key string) error             { return nil }
func (m *mockMergeStorage) IsExist(ctx context.Context, key string) (bool, error)    { return false, nil }
func (m *mockMergeStorage) ListObjects(ctx context.Context, prefix string) ([]string, error) {
	return nil, nil
}

// Compile-time check
var _ storage.Storage = (*mockMergeStorage)(nil)

func TestToggleSchedule(t *testing.T) {
	db := setupTestDB(t)
	srv := &APIServer{db: db}

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

	body := `{"enabled":false}`
	req := httptest.NewRequest(http.MethodPut, "/api/v1/schedule/schtoggle01/toggle", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String())
	}

	var mt model.MultiTasks
	db.Where("t_id = ?", "schtoggle01").First(&mt)
	if mt.Enabled {
		t.Fatal("expected schedule to be paused")
	}
}

func TestDeleteSchedule(t *testing.T) {
	db := setupTestDB(t)
	logger, _ := zap.NewProduction()
	srv := &APIServer{db: db, logger: logger}

	db.Create(&model.MultiTasks{
		TID:         "schdel01",
		TriggerType: model.TriggerCron,
		CronExpr:    "0 */5 * * * *",
		Enabled:     true,
	})

	gin.SetMode(gin.TestMode)
	r := gin.New()
	api := r.Group("/api/v1")
	api.DELETE("/schedule/:tid", srv.DeleteSchedule)

	req := httptest.NewRequest(http.MethodDelete, "/api/v1/schedule/schdel01", nil)
	w := httptest.NewRecorder()
	r.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", w.Code, w.Body.String())
	}

	var count int64
	db.Model(&model.MultiTasks{}).Where("tid = ?", "schdel01").Count(&count)
	if count != 0 {
		t.Fatalf("expected 0 schedules after delete, got %d", count)
	}
}
