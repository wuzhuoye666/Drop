package middleware

import (
	"bytes"
	"io"
	"time"

	"github.com/gin-gonic/gin"
	"go.uber.org/zap"
)

// bodyMaxLog is the threshold above which request bodies are truncated in logs.
const bodyMaxLog = 10 * 1024 // 10 KB

// AccessLog returns a Gin middleware that logs each request in structured JSON.
func AccessLog(logger *zap.Logger) gin.HandlerFunc {
	return func(c *gin.Context) {
		start := time.Now()

		// Capture request body for logging (limited)
		var bodyBuf bytes.Buffer
		if c.Request.Body != nil {
			limited := io.LimitReader(c.Request.Body, bodyMaxLog+1)
			tee := io.TeeReader(limited, &bodyBuf)
			bodyBytes, _ := io.ReadAll(tee)
			// Restore body for downstream handlers
			restored := io.MultiReader(&bodyBuf, c.Request.Body)
			c.Request.Body = io.NopCloser(restored)

			if len(bodyBytes) > bodyMaxLog {
				logger.Info("access",
					zap.String("method", c.Request.Method),
					zap.String("path", c.Request.URL.Path),
					zap.Int("status", c.Writer.Status()),
					zap.Int64("latency_ms", time.Since(start).Milliseconds()),
					zap.String("body", string(bodyBytes[:bodyMaxLog])+"...[truncated]"),
				)
				c.Next()
				return
			}
		}

		c.Next()

		logger.Info("access",
			zap.String("method", c.Request.Method),
			zap.String("path", c.Request.URL.Path),
			zap.Int("status", c.Writer.Status()),
			zap.Int64("latency_ms", time.Since(start).Milliseconds()),
		)
	}
}
