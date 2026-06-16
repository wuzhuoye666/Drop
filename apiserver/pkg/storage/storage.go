package storage

import (
	"context"
	"io"
)

// Storage defines the interface for object storage operations.
type Storage interface {
	// Put uploads a reader to the given key.
	Put(ctx context.Context, key string, reader io.Reader, size int64, contentType string) error
	// Get downloads an object by key.
	Get(ctx context.Context, key string) (io.ReadCloser, error)
	// PreSign generates a presigned URL for temporary access.
	PreSign(ctx context.Context, key string) (string, error)
	// Delete removes an object.
	Delete(ctx context.Context, key string) error
	// IsExist checks if an object exists.
	IsExist(ctx context.Context, key string) (bool, error)
	// ListObjects lists all objects with the given prefix.
	ListObjects(ctx context.Context, prefix string) ([]string, error)
}
