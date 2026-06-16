package server

import (
	"bytes"
	"context"
	"testing"

	"github.com/drop/apiserver/config"
	"github.com/drop/apiserver/pkg/storage/minio"
	"github.com/stretchr/testify/assert"
	"go.uber.org/zap"
)

func TestMinIOStorage(t *testing.T) {
	logger, _ := zap.NewDevelopment()
	cfg := config.S3Config{
		Endpoint:  "127.0.0.1:9000",
		AccessKey: "minioadmin",
		SecretKey: "minioadmin",
		Bucket:    "drop-data-test",
		UseSSL:    false,
		PreSignExp: 3600,
	}

	store, err := minio.NewStorage(cfg, logger)
	assert.NoError(t, err, "NewStorage should succeed")

	ctx := context.Background()
	key := "test/unit-test.txt"
	content := []byte("hello drop storage test")

	// Put
	err = store.Put(ctx, key, bytes.NewReader(content), int64(len(content)), "text/plain")
	assert.NoError(t, err, "Put should succeed")

	// IsExist
	exists, err := store.IsExist(ctx, key)
	assert.NoError(t, err, "IsExist should succeed")
	assert.True(t, exists, "object should exist after Put")

	// Get
	rc, err := store.Get(ctx, key)
	assert.NoError(t, err, "Get should succeed")
	var buf bytes.Buffer
	_, err = buf.ReadFrom(rc)
	assert.NoError(t, err, "Read should succeed")
	assert.Equal(t, content, buf.Bytes(), "content should match")
	rc.Close()

	// PreSign
	url, err := store.PreSign(ctx, key)
	assert.NoError(t, err, "PreSign should succeed")
	assert.NotEmpty(t, url, "presigned URL should not be empty")

	// Delete
	err = store.Delete(ctx, key)
	assert.NoError(t, err, "Delete should succeed")

	// Verify deleted
	exists, err = store.IsExist(ctx, key)
	assert.NoError(t, err, "IsExist after delete should succeed")
	assert.False(t, exists, "object should not exist after Delete")
}
