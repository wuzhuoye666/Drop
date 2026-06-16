package middleware

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/gin-gonic/gin"
	"github.com/stretchr/testify/assert"
	"go.uber.org/zap"
)

func TestAuth_RejectsWithoutUID(t *testing.T) {
	gin.SetMode(gin.TestMode)
	logger, _ := zap.NewDevelopment()

	r := gin.New()
	r.Use(Auth(false, logger))
	r.GET("/test", func(c *gin.Context) {
		c.JSON(200, gin.H{"ok": true})
	})

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/test", nil)
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusUnauthorized, w.Code)
}

func TestAuth_AcceptsCookie(t *testing.T) {
	gin.SetMode(gin.TestMode)
	logger, _ := zap.NewDevelopment()

	r := gin.New()
	r.Use(Auth(false, logger))
	r.GET("/test", func(c *gin.Context) {
		c.JSON(200, gin.H{"uid": c.GetString("uid")})
	})

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/test", nil)
	req.AddCookie(&http.Cookie{Name: "drop_user_uid", Value: "u123"})
	req.AddCookie(&http.Cookie{Name: "drop_user_name", Value: "tester"})
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusOK, w.Code)
}

func TestAuth_DevModeInjectsUser(t *testing.T) {
	gin.SetMode(gin.TestMode)
	logger, _ := zap.NewDevelopment()

	r := gin.New()
	r.Use(Auth(true, logger))
	r.GET("/test", func(c *gin.Context) {
		c.JSON(200, gin.H{"uid": c.GetString("uid")})
	})

	w := httptest.NewRecorder()
	req, _ := http.NewRequest("GET", "/test", nil)
	r.ServeHTTP(w, req)
	assert.Equal(t, http.StatusOK, w.Code)
}
