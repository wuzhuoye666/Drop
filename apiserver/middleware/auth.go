package middleware

import (
	"net/http"

	"github.com/gin-gonic/gin"
	"go.uber.org/zap"
)

// Auth returns a middleware that checks for drop_user_uid / drop_user_name cookies.
// In devMode, it injects a test user and skips the check.
func Auth(devMode bool, logger *zap.Logger) gin.HandlerFunc {
	return func(c *gin.Context) {
		uid, _ := c.Cookie("drop_user_uid")
		name, _ := c.Cookie("drop_user_name")

		if devMode && uid == "" {
			uid = "dev-uid"
			name = "developer"
		}

		if uid == "" {
			logger.Warn("auth failed: no uid cookie")
			c.JSON(http.StatusUnauthorized, gin.H{
				"code": 4010001,
				"data": gin.H{
					"location": "/login",
				},
			})
			c.Abort()
			return
		}

		c.Set("uid", uid)
		c.Set("user_name", name)
		c.Next()
	}
}
