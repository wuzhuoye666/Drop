#!/usr/bin/env python3
"""hotmethod_analyzer.py — Perf data analysis entry point.

Usage:
    python3 hotmethod_analyzer.py --task-id <tid> --task-type 0 --config config.ini

Reads task params from PG, downloads perf.data from MinIO, generates
flame graph SVG + top.json, uploads results back to MinIO.
"""

import argparse
import configparser
import json
import os
import subprocess
import sys
import tempfile

import psycopg2
from minio import Minio


# ── PG Helpers ──────────────────────────────────────────────────────

def get_task_info(dsn: str, tid: str) -> dict:
    conn = psycopg2.connect(dsn)
    cur = conn.cursor()
    cur.execute(
        "SELECT tid, profiler_type, request_params, cos_key, analysis_status "
        "FROM hotmethod_task WHERE tid = %s", (tid,)
    )
    row = cur.fetchone()
    conn.close()
    if not row:
        print(f"ERROR: task {tid} not found in PG", file=sys.stderr)
        sys.exit(1)
    return {
        "tid": row[0],
        "profiler_type": row[1],
        "request_params": row[2],
        "cos_key": row[3],
        "analysis_status": row[4],
    }


def update_analysis_status(dsn: str, tid: str, status: int, reason: str = ""):
    conn = psycopg2.connect(dsn)
    cur = conn.cursor()
    cur.execute(
        "UPDATE hotmethod_task SET analysis_status = %s, status_info = %s WHERE tid = %s",
        (status, reason, tid),
    )
    conn.commit()
    conn.close()


# ── MinIO Helpers ───────────────────────────────────────────────────

def make_minio_client(cfg: configparser.SectionProxy) -> Minio:
    return Minio(
        cfg.get("endpoint", "127.0.0.1:9000"),
        access_key=cfg.get("access_key", "minioadmin"),
        secret_key=cfg.get("secret_key", "minioadmin"),
        secure=cfg.getboolean("secure", False),
    )


def download_file(mc: Minio, bucket: str, key: str, dest: str):
    mc.fget_object(bucket, key, dest)


def upload_file(mc: Minio, bucket: str, key: str, filepath: str, content_type: str = "application/octet-stream"):
    mc.fput_object(bucket, key, filepath, content_type=content_type)


# ── Analysis Pipelines ─────────────────────────────────────────────

def analyze_perf(tid: str, perf_data: str, work_dir: str) -> list:
    """perf.data → perf.script.txt → collapsed.txt → flamegraph.svg + top.json"""
    script_path = os.path.join(work_dir, "perf.script.txt")
    collapsed_path = os.path.join(work_dir, "collapsed.txt")
    svg_path = os.path.join(work_dir, "flamegraph.svg")
    top_path = os.path.join(work_dir, "top.json")

    # perf script
    r = subprocess.run(
        ["perf", "script", "-i", perf_data],
        capture_output=True, text=True, timeout=120,
    )
    if r.returncode != 0:
        raise RuntimeError(f"perf script failed: {r.stderr.strip()}")
    with open(script_path, "w") as f:
        f.write(r.stdout)

    # Find stackcollapse-perf.pl
    collapse_tool = _find_tool("stackcollapse-perf.pl")
    with open(script_path) as fin, open(collapsed_path, "w") as fout:
        r = subprocess.run(["perl", collapse_tool], stdin=fin, stdout=fout, timeout=60)
        if r.returncode != 0:
            raise RuntimeError("stackcollapse-perf.pl failed")

    # FlameGraph.pl
    fg_tool = _find_tool("flamegraph.pl")
    with open(collapsed_path) as fin, open(svg_path, "w") as fout:
        r = subprocess.run(
            ["perl", fg_tool, "--title", "CPU Flame Graph", "--width", "1200"],
            stdin=fin, stdout=fout, timeout=60,
        )
        if r.returncode != 0:
            raise RuntimeError("flamegraph.pl failed")

    # TopN
    topn = parse_topn(collapsed_path)
    with open(top_path, "w") as f:
        json.dump(topn, f, indent=2, ensure_ascii=False)

    return ["perf.script.txt", "collapsed.txt", "flamegraph.svg", "top.json"]


def parse_topn(collapsed_path: str, top_n: int = 30) -> list:
    """Parse collapsed stack format → top functions by self time."""
    func_self = {}
    func_inclusive = {}
    total = 0
    with open(collapsed_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            # Format: stack;path count
            parts = line.rsplit(" ", 1)
            if len(parts) != 2:
                continue
            stack, count_str = parts
            try:
                count = int(count_str)
            except ValueError:
                continue
            total += count
            # Self = last function in stack
            frames = stack.split(";")
            self_func = frames[-1]
            func_self[self_func] = func_self.get(self_func, 0) + count
            # Inclusive = top of stack
            top_func = frames[0]
            func_inclusive[top_func] = func_inclusive.get(top_func, 0) + count

    if total == 0:
        return []

    result = []
    for func, self_val in sorted(func_self.items(), key=lambda x: -x[1])[:top_n]:
        inclusive_val = func_inclusive.get(func, self_val)
        result.append({
            "func": func,
            "self": self_val,
            "inclusive": inclusive_val,
            "percentage": round(self_val / total * 100, 2),
        })
    return result


def analyze_ebpf(tid: str, collapsed_path: str, work_dir: str) -> list:
    """eBPF collapsed stack → off-CPU flame graph + top.json"""
    svg_path = os.path.join(work_dir, "flamegraph_offcpu.svg")
    top_path = os.path.join(work_dir, "top.json")

    fg_tool = _find_tool("flamegraph.pl")
    with open(collapsed_path) as fin, open(svg_path, "w") as fout:
        r = subprocess.run(
            ["perl", fg_tool, "--color", "io",
             "--title", "Off-CPU Flame Graph", "--width", "1200",
             "--countname", "microseconds"],
            stdin=fin, stdout=fout, timeout=60,
        )
        if r.returncode != 0:
            raise RuntimeError("flamegraph.pl failed for eBPF data")

    topn = parse_topn(collapsed_path)
    with open(top_path, "w") as f:
        json.dump(topn, f, indent=2, ensure_ascii=False)

    return ["collapsed_ebpf.txt", "flamegraph_offcpu.svg", "top.json"]


def _find_tool(name: str) -> str:
    """Find FlameGraph tool in common locations."""
    candidates = [
        f"/opt/FlameGraph/{name}",
        f"/usr/local/bin/{name}",
        f"/root/FlameGraph/{name}",
        os.path.expanduser(f"~/FlameGraph/{name}"),
    ]
    # Also check PATH
    import shutil
    path_tool = shutil.which(name)
    if path_tool:
        return path_tool
    for c in candidates:
        if os.path.isfile(c):
            return c
    # Try to find it
    r = subprocess.run(["find", "/", "-name", name, "-type", "f"],
                       capture_output=True, text=True, timeout=10)
    for line in r.stdout.strip().split("\n"):
        if line:
            return line
    raise FileNotFoundError(f"Cannot find {name}. Install FlameGraph tools.")


# ── Main ────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Drop hotmethod analyzer")
    parser.add_argument("--task-id", required=True, help="Task TID")
    parser.add_argument("--task-type", type=int, default=0, help="0=perf, 3=ebpf")
    parser.add_argument("--config", default="config.ini", help="Config file")
    args = parser.parse_args()

    cfg = configparser.ConfigParser()
    cfg.read(args.config)

    dsn = cfg.get("pg", "dsn", fallback=os.getenv("PG_DSN", ""))
    if not dsn:
        print("ERROR: PG DSN not configured. Set [pg] dsn in config or PG_DSN env.", file=sys.stderr)
        sys.exit(1)

    # Mark analysis running
    update_analysis_status(dsn, args.task_id, 1, "分析开始")

    try:
        task = get_task_info(dsn, args.task_id)
        cos_key = task.get("cos_key", "")
        if not cos_key:
            raise RuntimeError(f"Task {args.task_id} has no cos_key (no upload data)")

        # Setup MinIO
        mc = make_minio_client(cfg["minio"])
        bucket = cfg.get("minio", "bucket", fallback="drop-data")

        with tempfile.TemporaryDirectory(prefix=f"drop_analysis_{args.task_id}_") as work_dir:
            # Download perf.data
            perf_data_path = os.path.join(work_dir, "perf.data")
            download_file(mc, bucket, cos_key, perf_data_path)
            print(f"Downloaded {cos_key} → {perf_data_path} "
                  f"({os.path.getsize(perf_data_path)} bytes)")

            # Analyze
            profiler_type = task.get("profiler_type", 0)
            if profiler_type == 0:
                artifacts = analyze_perf(args.task_id, perf_data_path, work_dir)
            elif profiler_type == 3:
                # eBPF: the uploaded file IS the collapsed stack
                collapsed_ebpf = os.path.join(work_dir, "collapsed_ebpf.txt")
                os.rename(perf_data_path, collapsed_ebpf)
                artifacts = analyze_ebpf(args.task_id, collapsed_ebpf, work_dir)
            else:
                raise RuntimeError(f"Unsupported profiler_type={profiler_type}")

            # Upload results
            for art in artifacts:
                art_path = os.path.join(work_dir, art)
                art_key = f"{args.task_id}/{art}"
                ct = "application/octet-stream"
                if art.endswith(".svg"):
                    ct = "image/svg+xml"
                elif art.endswith(".json"):
                    ct = "application/json"
                elif art.endswith(".txt"):
                    ct = "text/plain"
                upload_file(mc, bucket, art_key, art_path, ct)
                print(f"Uploaded {art_key}")

        update_analysis_status(dsn, args.task_id, 2, "分析完成")
        print(f"Analysis SUCCESS for {args.task_id}")
        sys.exit(0)

    except Exception as e:
        print(f"Analysis FAILED for {args.task_id}: {e}", file=sys.stderr)
        update_analysis_status(dsn, args.task_id, 3, f"分析失败: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
