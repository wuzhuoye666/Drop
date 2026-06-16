package middleware

import (
	"net/http"

	"github.com/gin-gonic/gin"
	"golang.org/x/time/rate"
)

// RateLimit returns a middleware that limits request rate using the provided limiter.
func RateLimit(limiter *rate.Limiter) gin.HandlerFunc {
	return func(c *gin.Context) {
		if !limiter.Allow() {
			c.JSON(http.StatusTooManyRequests, gin.H{
				"code":    4290001,
				"message": "rate limit exceeded",
			})
			c.Abort()
			return
		}
		c.Next()
	}
}
