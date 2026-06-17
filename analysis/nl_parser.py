#!/usr/bin/env python3
"""nl_parser.py — Parse natural language intent into structured profiling requests.

Extracts target PID, time range, profiler type, and other parameters
from conversational user input.
"""

import json
import re
import sys
from datetime import datetime, timedelta, timezone, timezone
from typing import Optional


# Profiler type keywords mapping
PROFILER_KEYWORDS = {
    0: ["cpu", "cpu采样", "perf", "cpu飙高", "cpu高", "cpu占用", "占用cpu", "cpu利用率"],
    1: ["java", "jvm", "async-profiler", "java采样", "java热点"],
    2: ["go", "pprof", "golang"],
    3: ["ebpf", "off-cpu", "offcpu", "io", "阻塞", "卡顿", "延迟", "io等待", "调度"],
}


def extract_pid(text: str) -> Optional[int]:
    """Extract target PID from natural language."""
    # Match "PID是1234", "进程1234", "pid 1234", "进程号1234", "PID 1234"
    patterns = [
        r'[Pp][Ii][Dd][是为：:\s]+(\d+)',
        r'进程[号为：:\s]*(\d+)',
        r'[Pp][Ii][Dd]\s+(\d+)',
        r'目标[是为：:\s]+(\d+)',
    ]
    for p in patterns:
        m = re.search(p, text)
        if m:
            return int(m.group(1))
    return None


def extract_profiler_type(text: str) -> Optional[int]:
    """Determine profiler type from text keywords."""
    text_lower = text.lower()
    for ptype, keywords in PROFILER_KEYWORDS.items():
        for kw in keywords:
            if kw in text_lower:
                return ptype
    # Default to CPU perf if ambiguous
    return None


def extract_time_range(text: str) -> Optional[dict]:
    """Extract relative or absolute time range from text.

    Returns {"start_ts": epoch_seconds, "end_ts": epoch_seconds} or None.
    """
    now = datetime.now(timezone.utc)

    # "过去N小时", "最近N小时", "最近N分钟", "过去一小时"
    m = re.search(r'(?:过去|最近)\s*一?(\d*)\s*(分钟|小时|天)', text)
    if m:
        val = int(m.group(1)) if m.group(1) else 1  # "一小时" → 1
        unit = m.group(2)
        if unit == "分钟":
            delta = timedelta(minutes=val)
        elif unit == "小时":
            delta = timedelta(hours=val)
        elif unit == "天":
            delta = timedelta(days=val)
        else:
            delta = timedelta(hours=1)
        start = now - delta
        return {"start_ts": int(start.timestamp()), "end_ts": int(now.timestamp())}

    # "上一小时", "上一个小时"
    if re.search(r'上一?个?小时', text):
        start = now - timedelta(hours=1)
        return {"start_ts": int(start.timestamp()), "end_ts": int(now.timestamp())}

    # "今天", "今天到现在"
    if "今天" in text:
        start = now.replace(hour=0, minute=0, second=0, microsecond=0)
        return {"start_ts": int(start.timestamp()), "end_ts": int(now.timestamp())}

    return None


def extract_duration(text: str) -> int:
    """Extract sampling duration in seconds."""
    # "采集10秒", "采样30秒", "持续10秒"
    m = re.search(r'(?:采集|采样|持续|时长)[为约：:\s]*(\d+)\s*秒', text)
    if m:
        return int(m.group(1))
    # "10秒钟", "30秒的"
    m = re.search(r'(\d+)\s*秒', text)
    if m:
        return int(m.group(1))
    return 10  # default 10 seconds


def extract_target_ip(text: str) -> Optional[str]:
    """Extract target IP from text."""
    m = re.search(r'(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})', text)
    if m:
        return m.group(1)
    return None


def parse_intent(text: str, available_agents: list = None) -> dict:
    """Parse natural language into a structured profiling request.

    Args:
        text: user's natural language input
        available_agents: list of {"ip_addr": ..., "hostname": ...} for fallback IP

    Returns:
        dict with keys: pid, profiler_type, duration, target_ip, time_range, raw_text
    """
    result = {
        "pid": extract_pid(text),
        "profiler_type": extract_profiler_type(text),
        "duration": extract_duration(text),
        "target_ip": extract_target_ip(text),
        "time_range": extract_time_range(text),
        "raw_text": text,
    }

    # Default profiler type to CPU perf if not detected
    if result["profiler_type"] is None:
        result["profiler_type"] = 0

    # If no IP found but agents available, use first agent
    if result["target_ip"] is None and available_agents:
        result["target_ip"] = available_agents[0].get("ip_addr", "127.0.0.1")

    # If no PID found, use 0 to indicate "profile system-wide" or prompt
    if result["pid"] is None:
        result["pid"] = 0

    return result


# ── CLI ──────────────────────────────────────────────────────────────

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Parse NL intent into profiling request")
    parser.add_argument("text", help="Natural language input")
    parser.add_argument("--agents", default="[]", help="JSON list of available agents")
    args = parser.parse_args()

    agents = json.loads(args.agents)
    result = parse_intent(args.text, agents)
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
