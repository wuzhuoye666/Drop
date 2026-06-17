#!/usr/bin/env python3
"""analysis_advisor.py — Rule-based & AI-powered analysis advisor.

Provides two modes:
  rule  — loads rules.yaml, matches TopN function names against regex patterns
  ai    — calls OpenAI-compatible LLM with tool definitions for structured diagnosis
"""

import configparser
import json
import os
import re
import sys
from typing import Optional

import yaml


# ── Rule Engine ─────────────────────────────────────────────────────

def load_rules(rules_path: str = "") -> list:
    """Load rules from YAML file."""
    if not rules_path:
        rules_path = os.path.join(os.path.dirname(__file__), "rules.yaml")
    if not os.path.isfile(rules_path):
        print(f"WARN: rules file not found at {rules_path}", file=sys.stderr)
        return []
    with open(rules_path) as f:
        data = yaml.safe_load(f)
    return data if isinstance(data, list) else []


def match_rules(topn: list, rules: list) -> list:
    """Match topn functions against rule regex patterns.

    Args:
        topn: list of {"func": ..., "percentage": ...}
        rules: list of {"regex": ..., "advice": ..., "severity": ...}

    Returns:
        list of {"func", "percentage", "advice", "severity"}
    """
    results = []
    for item in topn:
        func = item.get("func", "")
        pct = item.get("percentage", 0)
        for rule in rules:
            pattern = rule.get("regex", "")
            advice = rule.get("advice", "")
            severity = rule.get("severity", "info")
            if not pattern or not advice:
                continue
            try:
                if re.search(pattern, func, re.IGNORECASE):
                    results.append({
                        "func": func,
                        "percentage": pct,
                        "advice": advice,
                        "severity": severity,
                    })
                    break  # first matching rule wins per function
            except re.error:
                print(f"WARN: invalid regex in rule: {pattern}", file=sys.stderr)
                continue
    return results


def generate_suggestions_md(matches: list) -> str:
    """Render rule matches as Markdown."""
    if not matches:
        return "No rule-based suggestions for the top hot functions.\n"
    lines = ["# Rule-Based Analysis Suggestions\n"]
    severity_emoji = {"critical": "🔴", "warn": "🟡", "info": "🔵"}
    for m in matches:
        icon = severity_emoji.get(m["severity"], "🔵")
        lines.append(f"## {icon} `{m['func']}` ({m['percentage']}%)\n")
        lines.append(f"**Severity:** {m['severity']}\n")
        lines.append(f"{m['advice']}\n")
    return "\n".join(lines)


def run_rule_engine(topn_path: str, rules_path: str = "", output_path: str = "") -> str:
    """Run rule engine on a top.json file, write suggestions.md."""
    with open(topn_path) as f:
        topn = json.load(f)
    rules = load_rules(rules_path)
    matches = match_rules(topn, rules)
    md = generate_suggestions_md(matches)
    if not output_path:
        output_path = os.path.join(os.path.dirname(topn_path), "suggestions.md")
    with open(output_path, "w") as f:
        f.write(md)
    return md


# ── LLM Client ──────────────────────────────────────────────────────

def _get_llm_config(cfg_path: str) -> dict:
    """Read [llm] section from config.ini."""
    cfg = configparser.ConfigParser()
    cfg.read(cfg_path)
    return {
        "api_key": cfg.get("llm", "api_key", fallback=os.getenv("DROP_LLM_API_KEY", "")),
        "base_url": cfg.get("llm", "base_url", fallback=os.getenv("DROP_LLM_BASE_URL", "https://api.openai.com/v1")),
        "model": cfg.get("llm", "model", fallback=os.getenv("DROP_LLM_MODEL", "gpt-4o-mini")),
        "max_tokens": cfg.getint("llm", "max_tokens", fallback=2048),
        "temperature": cfg.getfloat("llm", "temperature", fallback=0.3),
    }


def ask_llm(topn: list, task_meta: dict, cfg_path: str = "") -> dict:
    """Call OpenAI-compatible LLM with tool-constrained output.

    Args:
        topn: top functions list
        task_meta: dict with tid, profiler_type, target_ip, duration, etc.
        cfg_path: path to config.ini with [llm] section

    Returns:
        {"diagnosis": str, "evidence": str, "recommendation": str, "confidence": float}
    """
    try:
        import requests
    except ImportError:
        return _fallback_diagnosis(topn)

    llm_cfg = _get_llm_config(cfg_path or os.path.join(os.path.dirname(__file__), "config.ini"))

    if not llm_cfg["api_key"]:
        print("WARN: LLM API key not configured, using fallback heuristic", file=sys.stderr)
        return _fallback_diagnosis(topn)

    # Build messages
    topn_summary = json.dumps(topn[:10], indent=2, ensure_ascii=False)
    system_prompt = (
        "You are an expert performance analyst. Given profiling data, provide a structured diagnosis.\n"
        "You MUST respond with valid JSON containing exactly these fields:\n"
        "- diagnosis: string, one-paragraph root cause analysis\n"
        "- evidence: string, specific function names and percentages supporting the diagnosis\n"
        "- recommendation: string, actionable next steps\n"
        "- confidence: float between 0 and 1\n\n"
        "Do NOT include any text outside the JSON object."
    )
    user_prompt = (
        f"Task metadata: {json.dumps(task_meta, ensure_ascii=False)}\n\n"
        f"Top hot functions:\n{topn_summary}\n\n"
        "Analyze this profiling data and provide structured diagnosis."
    )

    url = llm_cfg["base_url"].rstrip("/") + "/chat/completions"
    headers = {
        "Authorization": f"Bearer {llm_cfg['api_key']}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": llm_cfg["model"],
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "max_tokens": llm_cfg["max_tokens"],
        "temperature": llm_cfg["temperature"],
    }

    try:
        resp = requests.post(url, headers=headers, json=payload, timeout=60)
        resp.raise_for_status()
        content = resp.json()["choices"][0]["message"]["content"]
        # Strip markdown code fences if present
        content = content.strip()
        if content.startswith("```"):
            content = content.split("\n", 1)[1] if "\n" in content else content[3:]
        if content.endswith("```"):
            content = content[:-3]
        content = content.strip()
        result = json.loads(content)
        # Validate fields
        for field in ("diagnosis", "evidence", "recommendation", "confidence"):
            if field not in result:
                result[field] = "" if field != "confidence" else 0.0
        result["confidence"] = float(result.get("confidence", 0.0))
        result["confidence"] = max(0.0, min(1.0, result["confidence"]))
        return result
    except Exception as e:
        print(f"WARN: LLM call failed: {e}", file=sys.stderr)
        return _fallback_diagnosis(topn)


def _fallback_diagnosis(topn: list) -> dict:
    """Heuristic fallback when LLM is unavailable."""
    if not topn:
        return {
            "diagnosis": "No significant hot functions detected in this profile.",
            "evidence": "Top N list is empty.",
            "recommendation": "N/A",
            "confidence": 0.1,
        }

    top = topn[0]
    func = top.get("func", "unknown")
    pct = top.get("percentage", 0)

    diag = f"The hottest function is `{func}` consuming {pct}% of CPU samples. "
    if pct > 30:
        diag += "This is a severe hotspot that likely dominates performance."
    elif pct > 15:
        diag += "This is a moderate hotspot worth investigating."
    else:
        diag += "The profile appears relatively flat with no dominant hotspot."

    return {
        "diagnosis": diag,
        "evidence": f"`{func}` at {pct}% self-time",
        "recommendation": "Review the flame graph for the full call path leading to this function.",
        "confidence": 0.4,
    }


def generate_ai_suggestion_md(ai_result: dict) -> str:
    """Render AI diagnosis as Markdown."""
    lines = ["# AI-Powered Analysis\n"]
    lines.append(f"**Confidence:** {ai_result.get('confidence', 0):.0%}\n")
    lines.append("## Diagnosis\n")
    lines.append(f"{ai_result.get('diagnosis', 'N/A')}\n")
    lines.append("## Evidence\n")
    lines.append(f"{ai_result.get('evidence', 'N/A')}\n")
    lines.append("## Recommendation\n")
    lines.append(f"{ai_result.get('recommendation', 'N/A')}\n")
    return "\n".join(lines)


# ── CLI ──────────────────────────────────────────────────────────────

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Drop analysis advisor")
    sub = parser.add_subparsers(dest="cmd")

    rule_p = sub.add_parser("rule", help="Run rule-based analysis")
    rule_p.add_argument("--topn", required=True, help="Path to top.json")
    rule_p.add_argument("--rules", default="", help="Path to rules.yaml")
    rule_p.add_argument("--output", default="", help="Output suggestions.md path")

    ai_p = sub.add_parser("ai", help="Run AI-powered analysis")
    ai_p.add_argument("--topn", required=True, help="Path to top.json")
    ai_p.add_argument("--task-meta", default="{}", help="JSON task metadata")
    ai_p.add_argument("--config", default="", help="Path to config.ini")

    args = parser.parse_args()

    if args.cmd == "rule":
        md = run_rule_engine(args.topn, args.rules, args.output)
        print(md)
    elif args.cmd == "ai":
        with open(args.topn) as f:
            topn = json.load(f)
        task_meta = json.loads(args.task_meta)
        result = ask_llm(topn, task_meta, args.config)
        print(json.dumps(result, indent=2, ensure_ascii=False))
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
