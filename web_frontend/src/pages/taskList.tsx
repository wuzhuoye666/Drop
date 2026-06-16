import { useEffect, useState, useCallback } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { listTasks, createTask } from '../api';
import { taskStatusLabel, taskStatusColor, analysisStatusLabel, analysisStatusColor, profilerTypeName } from '../utils';

interface Task {
  tid: string;
  name: string;
  type: number;
  profiler_type: number;
  target_ip: string;
  status: number;
  status_info: string;
  analysis_status: number;
  create_time: string;
}

interface CreateForm {
  name: string;
  type: number;
  profiler_type: number;
  target_ip: string;
  pid: number;
  duration: number;
  hz: number;
}

const defaultForm: CreateForm = {
  name: 'cpu-profile',
  type: 0,
  profiler_type: 0,
  target_ip: '',
  pid: 0,
  duration: 10,
  hz: 99,
};

export default function TaskListPage() {
  const [searchParams] = useSearchParams();
  const [tasks, setTasks] = useState<Task[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(1);
  const [showModal, setShowModal] = useState(false);
  const [form, setForm] = useState<CreateForm>({ ...defaultForm, target_ip: searchParams.get('target_ip') || '' });

  const fetchTasks = useCallback(() => {
    listTasks(page).then(res => {
      setTasks(res.data.data.list ?? []);
      setTotal(res.data.data.total ?? 0);
    });
  }, [page]);

  useEffect(() => { fetchTasks(); }, [fetchTasks]);

  useEffect(() => {
    const id = setInterval(fetchTasks, 3000);
    return () => clearInterval(id);
  }, [fetchTasks]);

  const handleCreate = () => {
    createTask(form).then(() => {
      setShowModal(false);
      setForm({ ...defaultForm });
      setTimeout(fetchTasks, 500);
    });
  };

  return (
    <div style={{ maxWidth: 1200, margin: '0 auto', padding: '24px 16px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
        <h2 style={{ margin: 0 }}>Tasks</h2>
        <button
          onClick={() => setShowModal(true)}
          style={{ padding: '8px 16px', background: '#1890ff', color: '#fff', border: 'none', borderRadius: 4, cursor: 'pointer' }}
        >
          + New Sampling
        </button>
      </div>

      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 13 }}>
        <thead>
          <tr style={{ borderBottom: '2px solid #f0f0f0', textAlign: 'left' }}>
            <th style={{ padding: 8 }}>TID</th>
            <th style={{ padding: 8 }}>Name</th>
            <th style={{ padding: 8 }}>Profiler</th>
            <th style={{ padding: 8 }}>Target</th>
            <th style={{ padding: 8 }}>Status</th>
            <th style={{ padding: 8 }}>Analysis</th>
            <th style={{ padding: 8 }}>Created</th>
          </tr>
        </thead>
        <tbody>
          {tasks.map(t => (
            <tr key={t.tid} style={{ borderBottom: '1px solid #f0f0f0' }}>
              <td style={{ padding: 8 }}>
                <Link to={`/task/result?tid=${t.tid}`} style={{ color: '#1890ff', textDecoration: 'none' }}>
                  {t.tid.slice(0, 8)}
                </Link>
              </td>
              <td style={{ padding: 8 }}>{t.name}</td>
              <td style={{ padding: 8 }}>{profilerTypeName(t.profiler_type)}</td>
              <td style={{ padding: 8 }}>{t.target_ip}</td>
              <td style={{ padding: 8 }}>
                <span style={{ color: taskStatusColor(t.status), fontWeight: 500 }}>
                  {taskStatusLabel(t.status)}
                </span>
              </td>
              <td style={{ padding: 8 }}>
                <span style={{ color: analysisStatusColor(t.analysis_status), fontSize: 12 }}>
                  {analysisStatusLabel(t.analysis_status)}
                </span>
              </td>
              <td style={{ padding: 8, color: '#8c8c8c' }}>{new Date(t.create_time).toLocaleString()}</td>
            </tr>
          ))}
          {tasks.length === 0 && (
            <tr><td colSpan={7} style={{ padding: 24, textAlign: 'center', color: '#8c8c8c' }}>No tasks yet</td></tr>
          )}
        </tbody>
      </table>

      <div style={{ marginTop: 12, color: '#8c8c8c', fontSize: 12 }}>
        Total: {total} | Page: {page} {total > 20 && `| ${Math.ceil(total / 20)} pages`}
      </div>

      {showModal && (
        <div style={{
          position: 'fixed', top: 0, left: 0, right: 0, bottom: 0,
          background: 'rgba(0,0,0,0.4)', display: 'flex', alignItems: 'center', justifyContent: 'center',
          zIndex: 1000,
        }}
          onClick={e => { if (e.target === e.currentTarget) setShowModal(false); }}
        >
          <div style={{ background: '#fff', borderRadius: 8, padding: 24, minWidth: 420, boxShadow: '0 6px 16px rgba(0,0,0,0.12)' }}>
            <h3 style={{ margin: '0 0 16px' }}>New Sampling Task</h3>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
              <label style={{ fontSize: 13 }}>
                Task Name
                <input value={form.name} onChange={e => setForm({ ...form, name: e.target.value })}
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
              </label>
              <label style={{ fontSize: 13 }}>
                Profiler Type
                <select value={form.profiler_type} onChange={e => setForm({ ...form, profiler_type: Number(e.target.value) })}
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4 }}>
                  <option value={0}>CPU-perf</option>
                  <option value={1}>Java-async-profiler</option>
                  <option value={2}>Go-pprof</option>
                  <option value={3}>eBPF-offCPU</option>
                </select>
              </label>
              <label style={{ fontSize: 13 }}>
                Target Agent IP
                <input value={form.target_ip} onChange={e => setForm({ ...form, target_ip: e.target.value })}
                  placeholder="e.g. 10.0.0.1"
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
              </label>
              <label style={{ fontSize: 13 }}>
                Target PID
                <input type="number" value={form.pid} onChange={e => setForm({ ...form, pid: Number(e.target.value) })}
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
              </label>
              <div style={{ display: 'flex', gap: 12 }}>
                <label style={{ flex: 1, fontSize: 13 }}>
                  Duration (s)
                  <input type="number" value={form.duration} onChange={e => setForm({ ...form, duration: Number(e.target.value) })}
                    style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
                </label>
                <label style={{ flex: 1, fontSize: 13 }}>
                  Frequency (Hz)
                  <input type="number" value={form.hz} onChange={e => setForm({ ...form, hz: Number(e.target.value) })}
                    style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
                </label>
              </div>
            </div>
            <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 8, marginTop: 20 }}>
              <button onClick={() => setShowModal(false)}
                style={{ padding: '8px 16px', border: '1px solid #d9d9d9', borderRadius: 4, background: '#fff', cursor: 'pointer' }}>
                Cancel
              </button>
              <button onClick={handleCreate}
                style={{ padding: '8px 16px', background: '#1890ff', color: '#fff', border: 'none', borderRadius: 4, cursor: 'pointer' }}>
                Create
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
