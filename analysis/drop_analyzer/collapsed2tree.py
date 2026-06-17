#!/usr/bin/env python3
"""Convert collapsed stack text to JSON tree for D3 flame graph.

Input:  collapsed stack lines  (func1;func2;func3 42)
Output: {name, value, children} JSON tree, optionally gzip-compressed.
"""

import argparse
import gzip
import json
import sys
from collections import defaultdict
from typing import Any, Dict


def collapsed_to_tree(text: str, max_depth: int = 200) -> Dict[str, Any]:
    """Parse collapsed stack text into a tree dict.
    
    Args:
        text: Collapsed stack lines separated by newlines.
        max_depth: Maximum stack depth to avoid excessively deep trees.
    
    Returns:
        A dict with {name, value, children} structure suitable for d3-flame-graph.
    """
    root: Dict[str, Any] = {"name": "root", "value": 0, "children": []}

    for line in text.strip().splitlines():
        line = line.strip()
        if not line:
            continue
        
        last_space = line.rfind(" ")
        if last_space < 0:
            continue
        
        stack_str = line[:last_space]
        count_str = line[last_space + 1:]
        
        try:
            count = int(count_str)
        except ValueError:
            continue
        
        if count <= 0:
            continue

        frames = stack_str.split(";")
        if len(frames) > max_depth:
            frames = frames[:max_depth]

        node = root
        for frame in frames:
            child = None
            for c in node.get("children", []):
                if c["name"] == frame:
                    child = c
                    break
            if child is None:
                child = {"name": frame, "value": 0, "children": []}
                if "children" not in node:
                    node["children"] = []
                node["children"].append(child)
            node = child

        node["value"] = node.get("value", 0) + count

    # Propagate values upward
    _propagate(root)
    return root


def _propagate(node: Dict[str, Any]) -> int:
    """Recursively compute total values from children."""
    children = node.get("children", [])
    if not children:
        return node.get("value", 0)
    
    child_sum = sum(_propagate(c) for c in children)
    node["value"] = node.get("value", 0) + child_sum
    return node["value"]


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert collapsed stacks to JSON tree")
    parser.add_argument("input", help="Input collapsed stack file (or - for stdin)")
    parser.add_argument("-o", "--output", default="-", help="Output file (default: stdout)")
    parser.add_argument("-z", "--gzip", action="store_true", help="Gzip compress output")
    parser.add_argument("--max-depth", type=int, default=200, help="Max stack depth")
    args = parser.parse_args()

    if args.input == "-":
        text = sys.stdin.read()
    else:
        with open(args.input, "r") as f:
            text = f.read()

    tree = collapsed_to_tree(text, max_depth=args.max_depth)
    json_str = json.dumps(tree, separators=(",", ":"))

    if args.gzip:
        data = gzip.compress(json_str.encode("utf-8"))
        if args.output == "-":
            sys.stdout.buffer.write(data)
        else:
            with open(args.output, "wb") as f:
                f.write(data)
    else:
        if args.output == "-":
            print(json_str)
        else:
            with open(args.output, "w") as f:
                f.write(json_str)


if __name__ == "__main__":
    main()
