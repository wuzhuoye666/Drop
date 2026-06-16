"""Tests for drop_analyzer hotmethod analysis pipeline."""
import json
import os
import tempfile

import pytest

# Add parent to path so we can import the analyzer
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from drop_analyzer.hotmethod_analyzer import parse_topn, analyze_ebpf


# ── parse_topn unit tests ───────────────────────────────────────────

class TestParseTopn:
    """Pure-function unit tests for parse_topn."""

    def test_basic_single_frame(self):
        collapsed = "main 100\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
            f.write(collapsed)
            path = f.name
        try:
            result = parse_topn(path)
            assert len(result) == 1
            assert result[0]["func"] == "main"
            assert result[0]["self"] == 100
            assert result[0]["inclusive"] == 100
            assert result[0]["percentage"] == 100.0
        finally:
            os.unlink(path)

    def test_multi_frame_stack(self):
        collapsed = "schedule;__schedule;io_schedule 50\nschedule;__schedule;do_nanosleep 30\n"
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
            f.write(collapsed)
            path = f.name
        try:
            result = parse_topn(path)
            assert len(result) == 2
            # io_schedule should be top (50 self)
            assert result[0]["func"] == "io_schedule"
            assert result[0]["self"] == 50
            assert result[1]["func"] == "do_nanosleep"
            assert result[1]["percentage"] == pytest.approx(37.5, abs=0.1)
        finally:
            os.unlink(path)

    def test_empty_input(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
            f.write("")
            path = f.name
        try:
            result = parse_topn(path)
            assert result == []
        finally:
            os.unlink(path)

    def test_top_n_limit(self):
        lines = [f"func_{i} {100-i}" for i in range(50)]
        with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as f:
            f.write("\n".join(lines) + "\n")
            path = f.name
        try:
            result = parse_topn(path, top_n=10)
            assert len(result) == 10
        finally:
            os.unlink(path)


# ── analyze_ebpf integration test ────────────────────────────────────

class TestAnalyzeEbpf:
    """Test that analyze_ebpf produces correct artifacts."""

    @pytest.fixture
    def collapsed_file(self, tmp_path):
        """Create a sample collapsed stack file."""
        p = tmp_path / "collapsed_ebpf.txt"
        p.write_text(
            "__schedule;schedule;io_schedule;ext4_file_write_iter 200\n"
            "__schedule;schedule;do_nanosleep;hrtimer_nanosleep 100\n"
            "__schedule;schedule;io_schedule;do_blockdev_direct_IO 50\n"
        )
        return str(p)

    def test_produces_svg(self, collapsed_file, tmp_path):
        artifacts = analyze_ebpf("test-tid", collapsed_file, str(tmp_path))
        assert "flamegraph_offcpu.svg" in artifacts

        svg_path = tmp_path / "flamegraph_offcpu.svg"
        assert svg_path.exists()
        content = svg_path.read_text()
        assert "Off-CPU" in content, "SVG title should contain 'Off-CPU'"

    def test_produces_top_json(self, collapsed_file, tmp_path):
        artifacts = analyze_ebpf("test-tid", collapsed_file, str(tmp_path))
        assert "top.json" in artifacts

        top_path = tmp_path / "top.json"
        assert top_path.exists()
        data = json.loads(top_path.read_text())
        assert len(data) > 0
        # ext4_file_write_iter should be #1 with 200 self
        assert data[0]["func"] == "ext4_file_write_iter"
        assert data[0]["self"] == 200
