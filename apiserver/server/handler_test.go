package server

import (
	"testing"
)

func TestListSuggestions_HandlerExists(t *testing.T) {
	// Verify the handler method exists on APIServer
	s := &APIServer{}
	_ = s.ListSuggestions
}
