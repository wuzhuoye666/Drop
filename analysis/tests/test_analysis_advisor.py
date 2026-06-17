"""Tests for analysis_advisor (rule engine + LLM client) and nl_parser."""
import json
import os
import tempfile

import pytest

import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from analysis_advisor import load_rules, match_rules, generate_suggestions_md, ask_llm, _fallback_diagnosis, generate_ai_suggestion_md
from nl_parser import extract_pid, extract_profiler_type, extract_time_range, extract_duration, extract_target_ip, parse_intent


# ── Rule Engine Tests ───────────────────────────────────────────────

class TestLoadRules:
    def test_load_default_rules(self):
        rules_path = os.path.join(os.path.dirname(__file__), "..", "rules.yaml")
        rules = load_rules(rules_path)
        assert len(rules) > 0, "rules.yaml should contain at least one rule"
        for rule in rules:
            assert "regex" in rule
            assert "advice" in rule

    def test_load_missing_file(self):
        rules = load_rules("/nonexistent/rules.yaml")
        assert rules == []


class TestMatchRules:
    @pytest.fixture
    def sample_rules(self):
        return [
            {"regex": ".*malloc.*", "advice": "Consider jemalloc", "severity": "warn"},
            {"regex": ".*lock.*", "advice": "Lock contention detected", "severity": "critical"},
            {"regex": ".*memcpy.*", "advice": "Reduce memory copies", "severity": "info"},
        ]

    def test_match_malloc(self, sample_rules):
        topn = [{"func": "glibc_malloc", "percentage": 15}]
        matches = match_rules(topn, sample_rules)
        assert len(matches) == 1
        assert matches[0]["func"] == "glibc_malloc"
        assert "jemalloc" in matches[0]["advice"]

    def test_match_multiple(self, sample_rules):
        topn = [
            {"func": "malloc_internal", "percentage": 20},
            {"func": "pthread_mutex_lock", "percentage": 10},
            {"func": "main", "percentage": 5},
        ]
        matches = match_rules(topn, sample_rules)
        assert len(matches) == 2
        funcs = [m["func"] for m in matches]
        assert "malloc_internal" in funcs
        assert "pthread_mutex_lock" in funcs

    def test_no_match(self, sample_rules):
        topn = [{"func": "main", "percentage": 5}]
        matches = match_rules(topn, sample_rules)
        assert len(matches) == 0

    def test_empty_topn(self, sample_rules):
        matches = match_rules([], sample_rules)
        assert matches == []


class TestGenerateSuggestionsMd:
    def test_with_matches(self):
        matches = [
            {"func": "malloc", "percentage": 20, "advice": "Use jemalloc", "severity": "warn"},
        ]
        md = generate_suggestions_md(matches)
        assert "malloc" in md
        assert "jemalloc" in md

    def test_empty_matches(self):
        md = generate_suggestions_md([])
        assert "No rule-based suggestions" in md


class TestFallbackDiagnosis:
    def test_hot_function(self):
        topn = [{"func": "hot_func", "percentage": 35}]
        result = _fallback_diagnosis(topn)
        assert "hot_func" in result["diagnosis"]
        assert result["confidence"] > 0

    def test_empty(self):
        result = _fallback_diagnosis([])
        assert "No significant" in result["diagnosis"]


class TestGenerateAiSuggestionMd:
    def test_render(self):
        ai = {"diagnosis": "CPU bound", "evidence": "func at 40%", "recommendation": "Optimize func", "confidence": 0.8}
        md = generate_ai_suggestion_md(ai)
        assert "CPU bound" in md
        assert "80%" in md


# ── NL Parser Tests ─────────────────────────────────────────────────

class TestExtractPid:
    def test_pid_chinese(self):
        assert extract_pid("PID是1234的问题") == 1234

    def test_pid_english(self):
        assert extract_pid("pid 5678 cpu飙高") == 5678

    def test_process_number(self):
        assert extract_pid("进程号9999") == 9999

    def test_no_pid(self):
        assert extract_pid("看看CPU飙高的问题") is None


class TestExtractProfilerType:
    def test_cpu_keyword(self):
        assert extract_profiler_type("CPU飙高") == 0

    def test_ebpf_keyword(self):
        assert extract_profiler_type("io阻塞严重") == 3

    def test_java_keyword(self):
        assert extract_profiler_type("java热点分析") == 1

    def test_no_keyword(self):
        assert extract_profiler_type("帮我看看进程") is None


class TestExtractTimeRange:
    def test_recent_hours(self):
        result = extract_time_range("过去2小时CPU飙升")
        assert result is not None
        assert "start_ts" in result
        assert "end_ts" in result

    def test_recent_minutes(self):
        result = extract_time_range("最近30分钟")
        assert result is not None

    def test_last_hour(self):
        result = extract_time_range("上一小时")
        assert result is not None

    def test_no_time(self):
        assert extract_time_range("分析一下") is None


class TestExtractDuration:
    def test_explicit_seconds(self):
        assert extract_duration("采集30秒") == 30

    def test_implicit_seconds(self):
        assert extract_duration("看看10秒的数据") == 10

    def test_default(self):
        assert extract_duration("分析一下") == 10


class TestExtractTargetIp:
    def test_ipv4(self):
        assert extract_target_ip("127.0.0.1的问题") == "127.0.0.1"

    def test_no_ip(self):
        assert extract_target_ip("我的机器CPU高") is None


class TestParseIntent:
    def test_full_intent(self):
        result = parse_intent("过去一小时127.0.0.1上PID为1234的CPU飙高帮我看看")
        assert result["pid"] == 1234
        assert result["profiler_type"] == 0
        assert result["target_ip"] == "127.0.0.1"
        assert result["time_range"] is not None

    def test_minimal_intent(self):
        result = parse_intent("看看性能问题")
        assert result["profiler_type"] == 0  # default CPU
        assert result["duration"] == 10

    def test_with_agents_fallback(self):
        agents = [{"ip_addr": "10.0.0.1", "hostname": "node1"}]
        result = parse_intent("CPU高帮我看看", agents)
        assert result["target_ip"] == "10.0.0.1"
