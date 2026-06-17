import React, { useRef, useEffect, useState, useCallback } from 'react';
import d3flamegraph from 'd3-flame-graph';
import './d3-flamegraph.css';
import * as d3 from 'd3';
import pako from 'pako';

interface FlamegraphData {
  name: string;
  value: number;
  children?: FlamegraphData[];
}

interface FlameGraphProps {
  data: FlamegraphData | string;
  width?: number;
  height?: number;
  title?: string;
  onZoom?: (name: string) => void;
}

/** Parse collapsed stack text into {name, value, children} tree. */
function collapsedToTree(text: string): FlamegraphData {
  const root: FlamegraphData = { name: 'root', value: 0, children: [] };
  const lines = text.trim().split('\n');

  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    const lastSpace = trimmed.lastIndexOf(' ');
    if (lastSpace < 0) continue;
    const stack = trimmed.substring(0, lastSpace);
    const count = parseInt(trimmed.substring(lastSpace + 1), 10);
    if (isNaN(count) || count <= 0) continue;

    const frames = stack.split(';');
    let node = root;

    for (const frame of frames) {
      let child = node.children?.find((c: FlamegraphData) => c.name === frame);
      if (!child) {
        child = { name: frame, value: 0, children: [] };
        if (!node.children) node.children = [];
        node.children.push(child);
      }
      node = child;
    }
    node.value = (node.value || 0) + count;
  }

  // Propagate values up
  function propagate(n: FlamegraphData): number {
    if (!n.children || n.children.length === 0) return n.value || 0;
    let childSum = 0;
    for (const c of n.children) {
      childSum += propagate(c);
    }
    n.value = (n.value || 0) + childSum;
    return n.value;
  }
  propagate(root);

  return root;
}

/** Try to decompress gzip data, otherwise treat as raw text. */
function maybeDecompress(raw: string): string {
  try {
    // Convert string to Uint8Array for pako
    const bytes = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) {
      bytes[i] = raw.charCodeAt(i) & 0xff;
    }
    const decompressed = pako.ungzip(bytes, { to: 'string' });
    return decompressed;
  } catch {
    return raw;
  }
}

const FlameGraph: React.FC<FlameGraphProps> = ({ data, width = 1200, height = 600, title, onZoom }) => {
  const chartRef = useRef<HTMLDivElement>(null);
  const [searchTerm, setSearchTerm] = useState('');
  const [breadcrumb, setBreadcrumb] = useState<string[]>(['root']);
  const flameRef = useRef<any>(null);

  const handleSearch = useCallback(() => {
    if (flameRef.current && searchTerm) {
      try {
        flameRef.current.search(searchTerm);
      } catch {
        // search may fail if no match
      }
    }
  }, [searchTerm]);

  const handleReset = useCallback(() => {
    if (flameRef.current) {
      try {
        flameRef.current.resetZoom();
      } catch {
        // resetZoom may fail
      }
    }
    setBreadcrumb(['root']);
  }, []);

  useEffect(() => {
    if (!chartRef.current) return;

    // Build tree data
    let treeData: FlamegraphData;
    if (typeof data === 'string') {
      const decompressed = maybeDecompress(data);
      treeData = collapsedToTree(decompressed);
    } else {
      treeData = data;
    }

    // Clear previous
    d3.select(chartRef.current).selectAll('*').remove();

    const chart = d3flamegraph()
      .width(width)
      .height(height)
      .cellHeight(18)
      .minFrameSize(2)
      .transitionDuration(250)
      .title(title || '')
      .selfValue(false)
      .transitionEase(d3.easeCubicInOut)
      .onClick((d: FlamegraphData) => {
        setBreadcrumb(prev => [...prev, d.name]);
        onZoom?.(d.name);
      });

    flameRef.current = chart;

    // Render chart
    const selection = d3.select(chartRef.current);
    (selection as any).datum(treeData).call(chart);

    // Clean up
    return () => {
      if (chartRef.current) {
        d3.select(chartRef.current).selectAll('*').remove();
      }
    };
  }, [data, width, height, title, onZoom]);

  return (
    <div style={{ width: '100%' }}>
      {/* Toolbar */}
      <div style={{ display: 'flex', gap: 8, marginBottom: 8, alignItems: 'center', flexWrap: 'wrap' }}>
        <input
          type="text"
          placeholder="Search function name..."
          value={searchTerm}
          onChange={e => setSearchTerm(e.target.value)}
          onKeyDown={e => e.key === 'Enter' && handleSearch()}
          style={{ padding: '4px 8px', border: '1px solid #d9d9d9', borderRadius: 4, minWidth: 200 }}
        />
        <button onClick={handleSearch} style={{ padding: '4px 12px', cursor: 'pointer' }}>Search</button>
        <button onClick={handleReset} style={{ padding: '4px 12px', cursor: 'pointer' }}>Reset Zoom</button>
        {breadcrumb.length > 1 && (
          <span style={{ fontSize: 12, color: '#8c8c8c' }}>
            {breadcrumb.map((b, i) => (
              <span key={i}>
                {i > 0 && ' / '}
                <span
                  style={{ cursor: i < breadcrumb.length - 1 ? 'pointer' : 'default', color: i < breadcrumb.length - 1 ? '#1890ff' : undefined }}
                  onClick={() => {
                    setBreadcrumb(breadcrumb.slice(0, i + 1));
                    if (i === 0) handleReset();
                  }}
                >
                  {b}
                </span>
              </span>
            ))}
          </span>
        )}
      </div>

      {/* Chart */}
      <div ref={chartRef} style={{ width: '100%', overflow: 'auto' }} />
    </div>
  );
};

export default FlameGraph;
export { collapsedToTree, maybeDecompress };
