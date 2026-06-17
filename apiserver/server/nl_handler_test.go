package server

import (
	"testing"
)

func TestGoNLFallback_ParseCPU(t *testing.T) {
	s := &APIServer{}
	result := s.goNLFallback("过去一小时CPU飙高帮我看看")
	if pt, ok := result["profiler_type"].(int); !ok || pt != 0 {
		t.Errorf("expected profiler_type=0 (CPU), got %v", result["profiler_type"])
	}
}

func TestGoNLFallback_ParseEbpf(t *testing.T) {
	s := &APIServer{}
	result := s.goNLFallback("io阻塞的问题帮忙看一下")
	if pt, ok := result["profiler_type"].(int); !ok || pt != 3 {
		t.Errorf("expected profiler_type=3 (eBPF), got %v", result["profiler_type"])
	}
}

func TestGoNLFallback_ParseJava(t *testing.T) {
	s := &APIServer{}
	result := s.goNLFallback("JVM热点分析一下")
	if pt, ok := result["profiler_type"].(int); !ok || pt != 1 {
		t.Errorf("expected profiler_type=1 (Java), got %v", result["profiler_type"])
	}
}

func TestGoNLFallback_ParseDuration(t *testing.T) {
	s := &APIServer{}
	result := s.goNLFallback("采集30秒的数据")
	if dur, ok := result["duration"].(int); !ok || dur != 30 {
		t.Errorf("expected duration=30, got %v", result["duration"])
	}
}

func TestGoNLFallback_ParsePID(t *testing.T) {
	s := &APIServer{}
	result := s.goNLFallback("PID是1234有问题")
	if pid, ok := result["pid"].(int); !ok || pid != 1234 {
		t.Errorf("expected pid=1234, got %v", result["pid"])
	}
}

func TestGoNLFallback_Defaults(t *testing.T) {
	s := &APIServer{}
	result := s.goNLFallback("随意看看")
	if pt, ok := result["profiler_type"].(int); !ok || pt != 0 {
		t.Errorf("expected default profiler_type=0, got %v", result["profiler_type"])
	}
	if dur, ok := result["duration"].(int); !ok || dur != 10 {
		t.Errorf("expected default duration=10, got %v", result["duration"])
	}
}

func TestTruncate(t *testing.T) {
	if truncate("hello", 10) != "hello" {
		t.Error("should not truncate short string")
	}
	if len(truncate("hello world this is very long", 10)) != 10 {
		t.Error("should truncate to maxLen")
	}
}
