package main

import (
	"flag"
	"fmt"
	"net/http"

	"github.com/gin-gonic/gin"
	"go.uber.org/zap"
	"github.com/spf13/viper"
)

func main() {
	configPath := flag.String("c", "apiserver.yaml", "config file path")
	flag.Parse()

	// Load config with Viper
	viper.SetConfigFile(*configPath)
	viper.AutomaticEnv()
	if err := viper.ReadInConfig(); err != nil {
		fmt.Printf("warning: failed to read config: %v\n", err)
	}

	// Initialize Zap JSON logger
	logger, err := zap.NewProduction()
	if err != nil {
		panic(fmt.Sprintf("failed to init logger: %v", err))
	}
	defer logger.Sync()

	port := viper.GetString("server.port")
	if port == "" {
		port = ":8191"
	}

	// Create Gin router
	r := gin.New()
	r.Use(gin.Recovery())

	r.GET("/healthz", func(c *gin.Context) {
		c.JSON(http.StatusOK, gin.H{"status": "ok"})
	})

	logger.Info("apiserver starting", zap.String("addr", port))
	if err := r.Run(port); err != nil {
		logger.Fatal("failed to start server", zap.Error(err))
	}
}
