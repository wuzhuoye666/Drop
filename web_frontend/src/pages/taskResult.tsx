import { useEffect, useState, useCallback, useRef } from 'react';
import * as echarts from 'echarts';
import { useSearchParams, Link } from 'react-router-dom';
import { getTask, listCosFiles, listSegments, getProfileWindow, listSuggestions, nlChat, nlChatFollowup } from '../api';
import { taskStatusLabel, taskStatusColor, analysisStatusLabel, analysisStatusColor, profilerTypeName } from '../utils';
import FlameGraph from '../components/flamegraph/FlameGraph';

interface Task {
  tid: string;
  name: string;
  type: number;
  profiler_type: number;
  target_ip: string;
  request_params: string;
  status: number;
  status_info: string;
  analysis_status: number;
  uid: string;
  create_time: string;
  begin_time?: string;
  end_time?: string;
  cos_key: string;
}

interface CosFile {
  name: string;
  key: string;
  url: string;
}

interface Segment {
  id: number;
  tid: string;
  start_ts: number;
  end_ts: number;
  s3_key: string;
}

interface Suggestion {
  id: number;
  tid: string;
  func: string;
  suggestion: string;
  ai_suggestion: string;
  status: number; // 0=rule, 1=AI
}

export default function TaskResultPage() {
  const [searchParams] = useSearchParams();
  const tid = searchParams.get('tid') || '';
  const [task, setTask] = useState<Task | null>(null);
  const [files, setFiles] = useState<CosFile[]>([]);
  const [tab, setTab] = useState<'flamegraph' | 'timeline' | 'topn' | 'ai'>('flamegraph');
  const [segments, setSegments] = useState<Segment[]>([]);
  const [windowCollapsed, setWindowCollapsed] = useState<string | null>(null);
  const [windowLoading, setWindowLoading] = useState(false);
  const [treeCollapsed, setTreeCollapsed] = useState<string | null>(null);
  const [useD3Flame, setUseD3Flame] = useState(true);

  const isContinuous = task?.type === 1;

  const fetchTask = useCallback(() => {
    if (!tid) return;
    getTask(tid).then(res => {
      setTask(res.data.data.task);
      setFiles(res.data.data.files ?? []);
    });
  }, [tid]);

  useEffect(() => { fetchTask(); }, [fetchTask]);

  useEffect(() => {
    if (!tid) return;
    const id = setInterval(fetchTask, 3000);
    return () => clearInterval(id);
  }, [tid, fetchTask]);

  // Fetch segments for continuous tasks
  useEffect(() => {
    if (!tid || !isContinuous) return;
    const fetchSegs = () => {
      listSegments(tid).then(res => {
        setSegments(res.data.data ?? []);
      }).catch(() => {});
    };
    fetchSegs();
    const id = setInterval(fetchSegs, 10000);
    return () => clearInterval(id);
  }, [tid, isContinuous]);

  // Auto-switch to timeline tab for continuous tasks
  useEffect(() => {
    if (isContinuous && tab === 'flamegraph' && segments.length > 0) {
      setTab('timeline');
    }
  }, [isContinuous, segments.length, tab]);

  // Fetch tree/collapsed data for D3 flame graph
  useEffect(() => {
    if (!useD3Flame) return;
    const cf = files.find(f => f.name === 'flamegraph_tree.json.gz' || f.name === 'flamegraph_tree.json');
    if (cf) {
      fetch(cf.url)
        .then(r => r.text())
        .then(text => setTreeCollapsed(text))
        .catch(() => setUseD3Flame(false));
      return;
    }
    // Fallback to collapsed stacks
    const col = files.find(f => f.name === 'collapsed.txt');
    if (col && !treeCollapsed) {
      fetch(col.url)
        .then(r => r.text())
        .then(text => setTreeCollapsed(text))
        .catch(() => {});
    }
  }, [files, useD3Flame, treeCollapsed]);

  if (!task) return <div style={{ padding: 24 }}>Loading...</div>;

  const isEbpf = task.profiler_type === 3;
  const isAsyncProfiler = task.profiler_type === 1;
  const svgFile = files.find(f =>
    f.name === (isEbpf ? 'flamegraph_offcpu.svg' : isAsyncProfiler ? 'flamegraph_java.svg' : 'flamegraph.svg'));
  const topFile = files.find(f => f.name === 'top.json');

  const flameTabLabel = isEbpf ? 'Off-CPU Flame Graph' : isAsyncProfiler ? 'Java Flame Graph' : 'CPU Flame Graph';

  const handleWindowSelect = async (start: number, end: number) => {
    setWindowLoading(true);
    try {
      const res = await getProfileWindow(tid, start, end);
      setWindowCollapsed(res.data);
    } catch {
      setWindowCollapsed(null);
    }
    setWindowLoading(false);
  };

  return (
    <div style={{ maxWidth: 1200, margin: '0 auto', padding: '24px 16px' }}>
      <div style={{ marginBottom: 16 }}>
        <Link to="/tasks" style={{ color: '#1890ff', fontSize: 13 }}>&larr; Back to Tasks</Link>
      </div>

      <h2 style={{ margin: '0 0 16px' }}>Task: {task.name}</h2>

      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 8, fontSize: 13, marginBottom: 24, padding: 16, background: '#fafafa', borderRadius: 8 }}>
        <div><strong>TID:</strong> {task.tid}</div>
        <div><strong>Profiler:</strong> {profilerTypeName(task.profiler_type)}</div>
        <div><strong>Type:</strong> {isContinuous ? 'Continuous' : 'Single-shot'}</div>
        <div><strong>Target:</strong> {task.target_ip}</div>
        <div><strong>Status:</strong> <span style={{ color: taskStatusColor(task.status) }}>{taskStatusLabel(task.status)}</span></div>
        <div><strong>Analysis:</strong> <span style={{ color: analysisStatusColor(task.analysis_status) }}>{analysisStatusLabel(task.analysis_status)}</span></div>
        <div><strong>Info:</strong> {task.status_info}</div>
        <div><strong>Created:</strong> {new Date(task.create_time).toLocaleString()}</div>
        {task.begin_time && <div><strong>Started:</strong> {new Date(task.begin_time).toLocaleString()}</div>}
        {task.end_time && <div><strong>Finished:</strong> {new Date(task.end_time).toLocaleString()}</div>}
      </div>

      {/* Tabs */}
      <div style={{ display: 'flex', gap: 0, borderBottom: '2px solid #f0f0f0', marginBottom: 16 }}>
        {(['flamegraph', ...(isContinuous ? ['timeline'] as const : []), 'topn', 'ai'] as const).map(t => (
          <button
            key={t}
            onClick={() => setTab(t)}
            style={{
              padding: '8px 20px', border: 'none', background: 'none',
              cursor: 'pointer', borderBottom: tab === t ? '2px solid #1890ff' : 'none',
              color: tab === t ? '#1890ff' : '#595959', fontWeight: tab === t ? 600 : 400,
              marginBottom: -2,
            }}
          >
            {t === 'flamegraph' ? flameTabLabel : t === 'timeline' ? 'Timeline' : t === 'topn' ? 'Top N' : 'AI Suggestion'}
          </button>
        ))}
      </div>

      {/* Tab Content */}
      {tab === 'flamegraph' && (
        <div>
          {(useD3Flame && treeCollapsed) ? (
            <div>
              <div style={{ marginBottom: 8, display: 'flex', justifyContent: 'flex-end' }}>
                <button
                  onClick={() => setUseD3Flame(false)}
                  style={{ fontSize: 12, color: '#8c8c8c', cursor: 'pointer', background: 'none', border: '1px solid #d9d9d9', borderRadius: 4, padding: '2px 8px' }}
                >
                  Switch to SVG view
                </button>
              </div>
              <FlameGraph data={treeCollapsed} width={1100} height={560} title={`${flameTabLabel} (Interactive)`} />
            </div>
          ) : svgFile ? (
            <div>
              {!useD3Flame && files.find(f => f.name === 'collapsed.txt') && (
                <div style={{ marginBottom: 8, display: 'flex', justifyContent: 'flex-end' }}>
                  <button
                    onClick={() => { setUseD3Flame(true); }}
                    style={{ fontSize: 12, color: '#8c8c8c', cursor: 'pointer', background: 'none', border: '1px solid #d9d9d9', borderRadius: 4, padding: '2px 8px' }}
                  >
                    Switch to interactive view
                  </button>
                </div>
              )}
              <iframe
                src={svgFile.url}
                title="Flame Graph"
                style={{ width: '100%', height: 600, border: '1px solid #f0f0f0', borderRadius: 4 }}
              />
            </div>
          ) : (
            <p style={{ color: '#8c8c8c' }}>Flame graph not yet available (analysis pending or in progress)</p>
          )}
        </div>
      )}

      {tab === 'timeline' && isContinuous && (
        <TimelinePanel
          segments={segments}
          collapsed={windowCollapsed}
          loading={windowLoading}
          onWindowSelect={handleWindowSelect}
          profilerType={task.profiler_type}
        />
      )}

      {tab === 'topn' && (
        <div>
          {topFile ? (
            <TopNTable url={topFile.url} />
          ) : (
            <p style={{ color: '#8c8c8c' }}>Top N data not yet available</p>
          )}
        </div>
      )}

      {tab === 'ai' && (
        <AISuggestionPanel tid={tid} taskStatus={task.status} analysisStatus={task.analysis_status} />
      )}
    </div>
  );
}

// ── Timeline Panel with ECharts dataZoom ─────────────────────────────────

function TimelinePanel({ segments, collapsed, loading, onWindowSelect, profilerType }: {
  segments: Segment[];
  collapsed: string | null;
  loading: boolean;
  onWindowSelect: (start: number, end: number) => void;
  profilerType: number;
}) {
  const [rangeStart, setRangeStart] = useState<number>(0);
  const [rangeEnd, setRangeEnd] = useState<number>(0);
  const chartRef = useRef<HTMLDivElement>(null);
  const chartInstance = useRef<any>(null);

  if (segments.length === 0) {
    return <p style={{ color: '#8c8c8c', padding: 24, textAlign: 'center' }}>No profiling segments yet. Continuous profiling collects data every 5 minutes.</p>;
  }

  const minTs = segments[0].start_ts;
  const maxTs = segments[segments.length - 1].end_ts;
  const windowSize = 300;

  if (rangeStart === 0) {
    const end = maxTs;
    const start = Math.max(minTs, end - windowSize);
    // Use setTimeout to avoid React render-phase state update
    setTimeout(() => { setRangeStart(start); setRangeEnd(end); }, 0);
  }

  const handleApply = () => {
    onWindowSelect(rangeStart, rangeEnd);
  };

  const formatTime = (ts: number) => new Date(ts * 1000).toLocaleTimeString();

  // ECharts initialization
  useEffect(() => {
    if (!chartRef.current) return;
    const chart = echarts.init(chartRef.current);
    chartInstance.current = chart;

    // Build series data: each segment is a bar on the timeline
    const seriesData = segments.map((seg, i) => [
      seg.start_ts,
      seg.end_ts - seg.start_ts,
      seg.start_ts,
      `${formatTime(seg.start_ts)} - ${formatTime(seg.end_ts)}`
    ]);

    const option = {
      tooltip: {
        formatter: (params: any) => params.data[3],
      },
      grid: { top: 10, bottom: 80, left: 60, right: 30 },
      xAxis: {
        type: 'time',
        min: new Date(minTs * 1000),
        max: new Date(maxTs * 1000),
        axisLabel: { fontSize: 11 },
      },
      yAxis: { type: 'category', data: ['segments'], axisLabel: { show: false } },
      series: [{
        type: 'custom',
        renderItem: (_params: any, api: any) => {
          const start = api.value(0);
          const duration = api.value(1);
          const startPos = api.coord([start, 0]);
          const endPos = api.coord([start + duration, 0]);
          const height = 32;
          return {
            type: 'rect',
            transition: ['shape'],
            shape: { x: startPos[0], y: startPos[1] - height / 2, width: Math.max(endPos[0] - startPos[0], 2), height },
            style: { fill: '#1890ff', opacity: 0.7 },
          };
        },
        data: seriesData,
      }],
      dataZoom: [{
        type: 'slider',
        xAxisIndex: 0,
        start: 0,
        end: 100,
        minHeight: 30,
        labelFormatter: (val: number) => formatTime(val / 1000),
      }, {
        type: 'inside',
        xAxisIndex: 0,
      }],
    };

    chart.setOption(option);

    // Sync dataZoom with state
    chart.on('datazoom', (params: any) => {
      const opt = chart.getOption();
      const dz = (opt.dataZoom as any[])[0];
      if (dz && dz.startValue != null && dz.endValue != null) {
        const s = Math.floor(dz.startValue / 1000);
        const e = Math.floor(dz.endValue / 1000);
        setRangeStart(s);
        setRangeEnd(e);
      }
    });

    const onResize = () => chart.resize();
    window.addEventListener('resize', onResize);
    return () => {
      window.removeEventListener('resize', onResize);
      chart.dispose();
    };
  }, [segments]);

  // Update chart dataZoom when range changes externally
  useEffect(() => {
    if (!chartInstance.current || rangeStart === 0) return;
    chartInstance.current.dispatchAction({
      type: 'dataZoom',
      startValue: rangeStart * 1000,
      endValue: rangeEnd * 1000,
    });
  }, [rangeStart, rangeEnd]);

  return (
    <div>
      {/* ECharts timeline with dataZoom slider */}
      <div ref={chartRef} style={{ width: '100%', height: 200 }} />

      {/* Window info + Apply button */}
      <div style={{ display: 'flex', gap: 12, alignItems: 'center', marginTop: 8, marginBottom: 16, fontSize: 13 }}>
        <span>Selected window: {formatTime(rangeStart)} — {formatTime(rangeEnd)}</span>
        <button
          onClick={handleApply}
          disabled={loading}
          style={{
            padding: '6px 16px', background: '#1890ff', color: '#fff',
            border: 'none', borderRadius: 4, cursor: loading ? 'wait' : 'pointer',
          }}
        >
          {loading ? 'Loading...' : 'View Flame Graph'}
        </button>
      </div>

      {/* Merged flame graph display */}
      {collapsed && (
        <div style={{ border: '1px solid #f0f0f0', borderRadius: 4, padding: 16 }}>
          <h4 style={{ margin: '0 0 8px' }}>
            Merged Flame Graph ({formatTime(rangeStart)} &mdash; {formatTime(rangeEnd)})
          </h4>
          <CollapsedStacksViewer data={collapsed} profilerType={profilerType} />
        </div>
      )}
    </div>
  );
}

// ── Collapsed Stacks Viewer (basic flame graph from text) ──────────────

function CollapsedStacksViewer({ data, profilerType }: { data: string; profilerType: number }) {
  // Parse the collapsed stacks and render a simple bar visualization
  const lines = data.trim().split('\n').filter(l => l.trim());
  const stacks: { stack: string; count: number }[] = [];
  let totalCount = 0;

  for (const line of lines) {
    const lastSpace = line.lastIndexOf(' ');
    if (lastSpace < 0) continue;
    const stack = line.substring(0, lastSpace);
    const count = parseInt(line.substring(lastSpace + 1), 10);
    if (isNaN(count)) continue;
    stacks.push({ stack, count });
    totalCount += count;
  }

  // Sort by count descending and show top 30
  stacks.sort((a, b) => b.count - a.count);
  const top = stacks.slice(0, 30);

  const colorMap: Record<number, string> = {
    0: '#e74c3c',   // CPU-perf: red
    1: '#e67e22',   // Java: orange
    3: '#3498db',   // eBPF: blue
  };
  const barColor = colorMap[profilerType] || '#e74c3c';

  return (
    <div style={{ maxHeight: 500, overflow: 'auto' }}>
      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
        <thead>
          <tr style={{ borderBottom: '2px solid #f0f0f0', textAlign: 'left' }}>
            <th style={{ padding: 6 }}>#</th>
            <th style={{ padding: 6 }}>Stack</th>
            <th style={{ padding: 6 }}>Samples</th>
            <th style={{ padding: 6 }}>%</th>
          </tr>
        </thead>
        <tbody>
          {top.map((s, i) => {
            const pct = totalCount > 0 ? (s.count / totalCount * 100) : 0;
            return (
              <tr key={i} style={{ borderBottom: '1px solid #f0f0f0' }}>
                <td style={{ padding: 6 }}>{i + 1}</td>
                <td style={{ padding: 6, fontFamily: 'monospace', fontSize: 11, maxWidth: 600, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} title={s.stack}>
                  {s.stack}
                </td>
                <td style={{ padding: 6 }}>{s.count}</td>
                <td style={{ padding: 6 }}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
                    <div style={{ width: 60, height: 8, background: '#f0f0f0', borderRadius: 4, overflow: 'hidden' }}>
                      <div style={{ width: `${Math.min(pct, 100)}%`, height: '100%', background: barColor }} />
                    </div>
                    <span>{pct.toFixed(2)}%</span>
                  </div>
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      <p style={{ color: '#8c8c8c', fontSize: 12, marginTop: 8 }}>
        Showing top 30 of {stacks.length} unique stacks | Total samples: {totalCount}
      </p>
    </div>
  );
}

// ── Top N Table ─────────────────────────────────────────────────────────

function TopNTable({ url }: { url: string }) {
  const [data, setData] = useState<{ func: string; self: number; inclusive: number; percentage: number }[]>([]);

  useEffect(() => {
    fetch(url).then(r => r.json()).then(setData).catch(() => {});
  }, [url]);

  if (data.length === 0) return <p style={{ color: '#8c8c8c' }}>Loading top N...</p>;

  return (
    <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 13 }}>
      <thead>
        <tr style={{ borderBottom: '2px solid #f0f0f0', textAlign: 'left' }}>
          <th style={{ padding: 8 }}>#</th>
          <th style={{ padding: 8 }}>Function</th>
          <th style={{ padding: 8 }}>Self</th>
          <th style={{ padding: 8 }}>Inclusive</th>
          <th style={{ padding: 8 }}>%</th>
        </tr>
      </thead>
      <tbody>
        {data.map((d, i) => (
          <tr key={i} style={{ borderBottom: '1px solid #f0f0f0' }}>
            <td style={{ padding: 8 }}>{i + 1}</td>
            <td style={{ padding: 8, fontFamily: 'monospace', fontSize: 12 }}>{d.func}</td>
            <td style={{ padding: 8 }}>{d.self}</td>
            <td style={{ padding: 8 }}>{d.inclusive}</td>
            <td style={{ padding: 8 }}>
              <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
                <div style={{
                  width: 60, height: 8, background: '#f0f0f0', borderRadius: 4, overflow: 'hidden',
                }}>
                  <div style={{
                    width: `${Math.min(d.percentage, 100)}%`, height: '100%',
                    background: d.percentage > 20 ? '#ff4d4f' : d.percentage > 10 ? '#faad14' : '#52c41a',
                  }} />
                </div>
                <span>{d.percentage}%</span>
              </div>
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

// ── AI Suggestion Panel with NL Chat ────────────────────────────────

function AISuggestionPanel({ tid, taskStatus, analysisStatus }: {
  tid: string;
  taskStatus: number;
  analysisStatus: number;
}) {
  const [suggestions, setSuggestions] = useState<Suggestion[]>([]);
  const [chatInput, setChatInput] = useState('');
  const [chatMessages, setChatMessages] = useState<{ role: string; content: string }[]>([]);
  const [chatLoading, setChatLoading] = useState(false);
  const chatEndRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (analysisStatus !== 2) return;
    listSuggestions(tid).then(res => {
      setSuggestions(res.data.data ?? []);
    }).catch(() => {});
  }, [tid, analysisStatus]);

  useEffect(() => {
    chatEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [chatMessages]);

  const handleSend = async () => {
    if (!chatInput.trim() || chatLoading) return;
    const msg = chatInput.trim();
    setChatInput('');
    setChatMessages(prev => [...prev, { role: 'user', content: msg }]);
    setChatLoading(true);

    try {
      const res = await nlChat(msg, tid);
      const data = res.data.data;
      const reply = typeof data.content === 'string' ? data.content : JSON.stringify(data.content);
      setChatMessages(prev => [...prev, { role: 'assistant', content: reply }]);
      // If a new task was created, start SSE stream for progress
      if (data.tid && data.type === 'task_created') {
        const evtSource = new EventSource(`/api/v1/nl/stream/${data.tid}`, { withCredentials: true });
        evtSource.addEventListener('status', (e: MessageEvent) => {
          const status = JSON.parse(e.data);
          setChatMessages(prev => {
            const last = prev[prev.length - 1];
            if (last?.role === 'status') {
              return [...prev.slice(0, -1), { role: 'status', content: `任务状态: ${status.status_info}` }];
            }
            return [...prev, { role: 'status', content: `任务状态: ${status.status_info}` }];
          });
        });
        evtSource.addEventListener('complete', () => {
          evtSource.close();
          setChatMessages(prev => [...prev, { role: 'assistant', content: '任务分析完成！请查看上方建议。' }]);
          listSuggestions(tid || data.tid).then(res => setSuggestions(res.data.data ?? [])).catch(() => {});
          setChatLoading(false);
        });
        evtSource.addEventListener('error', (e: MessageEvent) => {
          evtSource.close();
          const errMsg = e.data ? JSON.parse(e.data) : '发生错误';
          setChatMessages(prev => [...prev, { role: 'assistant', content: `错误: ${errMsg}` }]);
          setChatLoading(false);
        });
        return;
      }
    } catch {
      setChatMessages(prev => [...prev, { role: 'assistant', content: '请求失败，请稍后重试。' }]);
    }
    setChatLoading(false);
  };

  // Separate rule and AI suggestions
  const ruleSugs = suggestions.filter(s => s.status === 0);
  const aiSummary = suggestions.find(s => s.status === 1);

  let aiParsed: { diagnosis?: string; evidence?: string; recommendation?: string; confidence?: number } | null = null;
  if (aiSummary?.ai_suggestion) {
    try { aiParsed = JSON.parse(aiSummary.ai_suggestion); } catch { aiParsed = null; }
  }

  const isAnalysisReady = analysisStatus === 2;

  return (
    <div>
      {/* Suggestions section */}
      {!isAnalysisReady && (
        <div style={{ padding: 24, color: '#8c8c8c', textAlign: 'center' }}>
          {analysisStatus === 1 ? '分析正在进行中...' : analysisStatus === 3 ? '分析失败' : '等待任务完成后开始分析'}
        </div>
      )}

      {isAnalysisReady && aiParsed && (
        <div style={{ marginBottom: 24, padding: 16, background: '#f0f5ff', borderRadius: 8, border: '1px solid #adc6ff' }}>
          <h4 style={{ margin: '0 0 8px', color: '#1890ff' }}>AI Diagnosis</h4>
          <p style={{ margin: '0 0 8px' }}><strong>Diagnosis:</strong> {aiParsed.diagnosis}</p>
          <p style={{ margin: '0 0 8px' }}><strong>Evidence:</strong> {aiParsed.evidence}</p>
          <p style={{ margin: '0 0 8px' }}><strong>Recommendation:</strong> {aiParsed.recommendation}</p>
          <p style={{ margin: 0 }}><strong>Confidence:</strong> {aiParsed.confidence != null ? `${(aiParsed.confidence * 100).toFixed(0)}%` : 'N/A'}</p>
        </div>
      )}

      {isAnalysisReady && ruleSugs.length > 0 && (
        <div style={{ marginBottom: 24 }}>
          <h4 style={{ margin: '0 0 12px' }}>Rule-Based Suggestions</h4>
          {ruleSugs.map((s, i) => (
            <div key={i} style={{ padding: 12, marginBottom: 8, background: '#fafafa', borderRadius: 4, borderLeft: '3px solid #faad14' }}>
              <strong style={{ fontFamily: 'monospace', fontSize: 12 }}>{s.func}</strong>
              <p style={{ margin: '4px 0 0', fontSize: 13, color: '#595959' }}>{s.suggestion}</p>
            </div>
          ))}
        </div>
      )}

      {isAnalysisReady && ruleSugs.length === 0 && !aiParsed && (
        <div style={{ padding: 24, color: '#8c8c8c', textAlign: 'center' }}>
          No suggestions available for this profile. Try using the chat below.
        </div>
      )}

      {/* NL Chat */}
      <div style={{ borderTop: '1px solid #f0f0f0', paddingTop: 16 }}>
        <h4 style={{ margin: '0 0 12px' }}>Ask AI</h4>
        <div style={{
          height: 200, overflowY: 'auto', border: '1px solid #f0f0f0',
          borderRadius: 4, padding: 8, marginBottom: 8, fontSize: 13,
        }}>
          {chatMessages.length === 0 && (
            <div style={{ color: '#8c8c8c', textAlign: 'center', padding: 16 }}>
              Try: &quot;过去一小时CPU飙高帮我看看&quot; or &quot;分析这个任务的性能问题&quot;
            </div>
          )}
          {chatMessages.map((m, i) => (
            <div key={i} style={{
              marginBottom: 8, textAlign: m.role === 'user' ? 'right' : 'left',
            }}>
              <span style={{
                display: 'inline-block', maxWidth: '80%', padding: '6px 10px',
                borderRadius: 8, fontSize: 13,
                background: m.role === 'user' ? '#1890ff' : '#f0f0f0',
                color: m.role === 'user' ? '#fff' : '#333',
              }}>
                {m.content}
              </span>
            </div>
          ))}
          <div ref={chatEndRef} />
        </div>
        <div style={{ display: 'flex', gap: 8 }}>
          <input
            value={chatInput}
            onChange={e => setChatInput(e.target.value)}
            onKeyDown={e => e.key === 'Enter' && handleSend()}
            placeholder="Describe what you want to analyze..."
            style={{
              flex: 1, padding: '8px 12px', border: '1px solid #d9d9d9',
              borderRadius: 4, fontSize: 13, outline: 'none',
            }}
          />
          <button
            onClick={handleSend}
            disabled={chatLoading}
            style={{
              padding: '8px 16px', background: '#1890ff', color: '#fff',
              border: 'none', borderRadius: 4, cursor: chatLoading ? 'wait' : 'pointer',
            }}
          >
            Send
          </button>
        </div>
      </div>
    </div>
  );
}
