package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"time"

	"github.com/drop/apiserver/config"
	"github.com/drop/apiserver/middleware"
	"github.com/drop/apiserver/model"
	"github.com/drop/apiserver/pkg/storage"
	miniostore "github.com/drop/apiserver/pkg/storage/minio"
	"github.com/drop/apiserver/server"
	"github.com/gin-gonic/gin"
	"go.uber.org/zap"
	"golang.org/x/time/rate"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"gorm.io/driver/postgres"
	"gorm.io/gorm"
	gormlogger "gorm.io/gorm/logger"
)

func main() {
	configPath := flag.String("c", "apiserver.yaml", "config file path")
	devMode := flag.Bool("dev-mode", false, "skip auth, inject test user")
	flag.Parse()

	// Load config
	cfg, err := config.Load(*configPath)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}
	if *devMode {
		cfg.Server.DevMode = true
	}

	// Initialize Zap JSON logger
	logger, err := zap.NewProduction()
	if err != nil {
		log.Fatalf("init logger: %v", err)
	}
	defer logger.Sync()

	// Connect to PostgreSQL
	db, err := initDB(cfg, logger)
	if err != nil {
		logger.Fatal("connect database", zap.Error(err))
	}
	logger.Info("database connected")

	// AutoMigrate all models
	if err := db.AutoMigrate(
		&model.UserInfo{},
		&model.AgentInfo{},
		&model.HotmethodTask{},
		&model.MultiTasks{},
		&model.Group{},
		&model.GroupMember{},
		&model.AnalysisSuggestion{},
		&model.TaskStatusHistory{},
		&model.AgentAuditLog{},
	); err != nil {
		logger.Fatal("auto migrate", zap.Error(err))
	}
	logger.Info("database migrated")

	// Connect to gRPC server (drop_server)
	var grpcConn *grpc.ClientConn
	if cfg.GRPC.Addr != "" {
		grpcConn, err = grpc.NewClient(cfg.GRPC.Addr,
			grpc.WithTransportCredentials(insecure.NewCredentials()),
		)
		if err != nil {
			logger.Warn("grpc dial failed, running in mock mode", zap.Error(err))
		}
	}

	// Initialize MinIO storage
	var objStore storage.Storage
	if cfg.S3.Endpoint != "" {
		minioStore, err := miniostore.NewStorage(cfg.S3, logger)
		if err != nil {
			logger.Warn("minio init failed, storage unavailable", zap.Error(err))
		} else {
			objStore = minioStore
		}
	}

	// Create APIServer
	srv := server.New(db, grpcConn, objStore, cfg, logger)

	// Start analysis scheduler
	srv.StartAnalysisScheduler()

	// Create Gin router
	if cfg.Server.DevMode {
		gin.SetMode(gin.TestMode)
	}
	r := gin.New()
	r.Use(gin.Recovery())
	r.Use(middleware.AccessLog(logger))
	r.Use(middleware.CORS())
	r.Use(middleware.RateLimit(rate.NewLimiter(rate.Every(100*time.Millisecond), 50)))

	// Health check (no auth)
	r.GET("/healthz", func(c *gin.Context) {
		c.JSON(http.StatusOK, gin.H{"status": "ok"})
	})

	// Auth check
	r.GET("/api/v1/auth/check", srv.AuthCheck)

	// Agent internal endpoints (no cookie auth, outside api group)
	r.POST("/api/v1/tasks/:tid/upload/*filename", srv.UploadTaskFile)

	// API group with auth middleware
	api := r.Group("/api/v1")
	api.Use(middleware.Auth(cfg.Server.DevMode, logger))
	{
		// User
		api.GET("/users", srv.GetUser)

		// Agents
		api.GET("/agents", srv.ListAgents)
		api.GET("/agent/stat", srv.StatAgent)

		// Tasks
		api.POST("/tasks", srv.CreateTask)
		api.GET("/tasks", srv.ListTasks)
		api.GET("/tasks/:tid", srv.GetTask)
		api.DELETE("/tasks/:tid", srv.DeleteTask)
		api.POST("/tasks/:tid/retry", srv.RetryTask)

		// COS files
		api.GET("/cosfiles", srv.ListCosFiles)

		// Groups
		api.POST("/group", srv.CreateGroup)
		api.GET("/groups", srv.ListGroups)
		api.POST("/group/member", srv.AddGroupMember)
		api.DELETE("/group/member", srv.RemoveGroupMember)
		api.POST("/group/agent", srv.AddGroupAgent)
		api.DELETE("/group/agent", srv.RemoveGroupAgent)

		// Schedule
		api.POST("/schedule/task", srv.CreateScheduleTask)
	}
	addr := cfg.Server.Port
	logger.Info("apiserver starting", zap.String("addr", addr))
	if err := r.Run(addr); err != nil {
		logger.Fatal("failed to start server", zap.Error(err))
	}
}

func initDB(cfg *config.Config, logger *zap.Logger) (*gorm.DB, error) {
	gormCfg := &gorm.Config{
		Logger: gormlogger.Default.LogMode(gormlogger.Silent),
	}
	db, err := gorm.Open(postgres.Open(cfg.PG.DSN), gormCfg)
	if err != nil {
		return nil, fmt.Errorf("open: %w", err)
	}

	sqlDB, err := db.DB()
	if err != nil {
		return nil, fmt.Errorf("get underlying db: %w", err)
	}
	sqlDB.SetMaxOpenConns(cfg.PG.MaxOpenConns)
	sqlDB.SetMaxIdleConns(cfg.PG.MaxIdleConns)
	sqlDB.SetConnMaxLifetime(time.Duration(cfg.PG.ConnMaxLifetime) * time.Second)

	return db, nil
}
