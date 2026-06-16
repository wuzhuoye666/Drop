import { useEffect, useState } from 'react';
import { useSearchParams, Link } from 'react-router-dom';
import { getTask, listCosFiles } from '../api';
import { taskStatusLabel, taskStatusColor, analysisStatusLabel, analysisStatusColor, profilerTypeName } from '../utils';

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

export default function TaskResultPage() {
  const [searchParams] = useSearchParams();
  const tid = searchParams.get('tid') || '';
  const [task, setTask] = useState<Task | null>(null);
  const [files, setFiles] = useState<CosFile[]>([]);
  const [tab, setTab] = useState<'flamegraph' | 'topn' | 'ai'>('flamegraph');

  useEffect(() => {
    if (!tid) return;
    getTask(tid).then(res => {
      setTask(res.data.data.task);
      setFiles(res.data.data.files ?? []);
    });
  }, [tid]);

  useEffect(() => {
    if (!tid) return;
    const id = setInterval(() => {
      getTask(tid).then(res => {
        setTask(res.data.data.task);
        setFiles(res.data.data.files ?? []);
      });
    }, 3000);
    return () => clearInterval(id);
  }, [tid]);

  if (!task) return <div style={{ padding: 24 }}>Loading...</div>;

  const svgFile = files.find(f => f.name === 'flamegraph.svg');
  const topFile = files.find(f => f.name === 'top.json');

  return (
    <div style={{ maxWidth: 1200, margin: '0 auto', padding: '24px 16px' }}>
      <div style={{ marginBottom: 16 }}>
        <Link to="/tasks" style={{ color: '#1890ff', fontSize: 13 }}>← Back to Tasks</Link>
      </div>

      <h2 style={{ margin: '0 0 16px' }}>Task: {task.name}</h2>

      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 8, fontSize: 13, marginBottom: 24, padding: 16, background: '#fafafa', borderRadius: 8 }}>
        <div><strong>TID:</strong> {task.tid}</div>
        <div><strong>Profiler:</strong> {profilerTypeName(task.profiler_type)}</div>
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
        {(['flamegraph', 'topn', 'ai'] as const).map(t => (
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
            {t === 'flamegraph' ? 'Flame Graph' : t === 'topn' ? 'Top N' : 'AI Suggestion'}
          </button>
        ))}
      </div>

      {/* Tab Content */}
      {tab === 'flamegraph' && (
        <div>
          {svgFile ? (
            <iframe
              src={svgFile.url}
              title="Flame Graph"
              style={{ width: '100%', height: 600, border: '1px solid #f0f0f0', borderRadius: 4 }}
            />
          ) : (
            <p style={{ color: '#8c8c8c' }}>Flame graph not yet available (analysis pending or in progress)</p>
          )}
        </div>
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
        <div style={{ padding: 24, color: '#8c8c8c', textAlign: 'center' }}>
          AI suggestion engine coming soon (Phase 8)
        </div>
      )}
    </div>
  );
}

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
