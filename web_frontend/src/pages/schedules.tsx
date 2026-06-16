import { useEffect, useState, useCallback } from 'react';
import { Link } from 'react-router-dom';
import { listSchedules, createScheduleTask, toggleSchedule, deleteSchedule } from '../api';

interface Schedule {
  tid: string;
  sub_tids: string;
  type: number;
  status: number;
  trigger_type: number;
  cron_expr: string;
  enabled: boolean;
  created_at: string;
}

const typeNames: Record<number, string> = { 0: 'Single', 1: 'Continuous', 2: 'Multi' };

export default function SchedulesPage() {
  const [schedules, setSchedules] = useState<Schedule[]>([]);
  const [showModal, setShowModal] = useState(false);
  const [cronExpr, setCronExpr] = useState('0 */5 * * * *'); // every 5 min
  const [name, setName] = useState('continuous-profile');
  const [targetIp, setTargetIp] = useState('');
  const [pid, setPid] = useState(0);
  const [profilerType, setProfilerType] = useState(0);
  const [hz, setHz] = useState(1);

  const fetchSchedules = useCallback(() => {
    listSchedules().then(res => {
      setSchedules(res.data.data ?? []);
    }).catch(() => {});
  }, []);

  useEffect(() => { fetchSchedules(); }, [fetchSchedules]);

  useEffect(() => {
    const id = setInterval(fetchSchedules, 15000);
    return () => clearInterval(id);
  }, [fetchSchedules]);

  const handleCreate = () => {
    createScheduleTask({
      name,
      cron_expr: cronExpr,
      type: 1, // continuous
      profiler_type: profilerType,
      target_ip: targetIp,
      pid,
      hz,
    }).then(() => {
      setShowModal(false);
      setTimeout(fetchSchedules, 500);
    });
  };

  const handleToggle = (tid: string, enabled: boolean) => {
    toggleSchedule(tid, !enabled).then(() => {
      setTimeout(fetchSchedules, 300);
    });
  };

  return (
    <div style={{ maxWidth: 1200, margin: '0 auto', padding: '24px 16px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
        <h2 style={{ margin: 0 }}>Schedules</h2>
        <button
          onClick={() => setShowModal(true)}
          style={{ padding: '8px 16px', background: '#1890ff', color: '#fff', border: 'none', borderRadius: 4, cursor: 'pointer' }}
        >
          + New Schedule
        </button>
      </div>

      <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 13 }}>
        <thead>
          <tr style={{ borderBottom: '2px solid #f0f0f0', textAlign: 'left' }}>
            <th style={{ padding: 8 }}>TID</th>
            <th style={{ padding: 8 }}>Name</th>
            <th style={{ padding: 8 }}>Cron</th>
            <th style={{ padding: 8 }}>Sub Tasks</th>
            <th style={{ padding: 8 }}>Enabled</th>
            <th style={{ padding: 8 }}>Actions</th>
          </tr>
        </thead>
        <tbody>
          {schedules.map(s => {
            let subCount = 0;
            try { subCount = JSON.parse(s.sub_tids || '[]').length; } catch {}
            return (
              <tr key={s.tid} style={{ borderBottom: '1px solid #f0f0f0' }}>
                <td style={{ padding: 8 }}>{s.tid.slice(0, 8)}</td>
                <td style={{ padding: 8 }}>{s.cron_expr || '-'}</td>
                <td style={{ padding: 8, fontFamily: 'monospace' }}>{s.cron_expr}</td>
                <td style={{ padding: 8 }}>{subCount}</td>
                <td style={{ padding: 8 }}>
                  <span style={{ color: s.enabled ? '#52c41a' : '#ff4d4f' }}>
                    {s.enabled ? 'Active' : 'Paused'}
                  </span>
                </td>
                <td style={{ padding: 8 }}>
                  <button
                    onClick={() => handleToggle(s.tid, s.enabled)}
                    style={{
                      padding: '4px 12px', fontSize: 12, border: '1px solid #d9d9d9',
                      borderRadius: 4, background: '#fff', cursor: 'pointer',
                    }}
                  >
                    {s.enabled ? 'Pause' : 'Resume'}
                  </button>
                  <button
                    onClick={() => { if (confirm('Delete this schedule?')) deleteSchedule(s.tid).then(() => setTimeout(fetchSchedules, 300)); }}
                    style={{
                      padding: '4px 12px', fontSize: 12, border: '1px solid #ff4d4f',
                      borderRadius: 4, background: '#fff', color: '#ff4d4f', cursor: 'pointer', marginLeft: 4,
                    }}
                  >
                    Delete
                  </button>
                </td>
              </tr>
            );
          })}
          {schedules.length === 0 && (
            <tr><td colSpan={6} style={{ padding: 24, textAlign: 'center', color: '#8c8c8c' }}>No schedules yet</td></tr>
          )}
        </tbody>
      </table>

      {showModal && (
        <div style={{
          position: 'fixed', top: 0, left: 0, right: 0, bottom: 0,
          background: 'rgba(0,0,0,0.4)', display: 'flex', alignItems: 'center', justifyContent: 'center',
          zIndex: 1000,
        }}
          onClick={e => { if (e.target === e.currentTarget) setShowModal(false); }}
        >
          <div style={{ background: '#fff', borderRadius: 8, padding: 24, minWidth: 440, boxShadow: '0 6px 16px rgba(0,0,0,0.12)' }}>
            <h3 style={{ margin: '0 0 16px' }}>New Continuous Profiling Schedule</h3>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
              <label style={{ fontSize: 13 }}>
                Schedule Name
                <input value={name} onChange={e => setName(e.target.value)}
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
              </label>
              <label style={{ fontSize: 13 }}>
                Cron Expression (with seconds)
                <input value={cronExpr} onChange={e => setCronExpr(e.target.value)}
                  placeholder="0 */5 * * * * = every 5 minutes"
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
              </label>
              <label style={{ fontSize: 13 }}>
                Profiler Type
                <select value={profilerType} onChange={e => setProfilerType(Number(e.target.value))}
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4 }}>
                  <option value={0}>CPU-perf</option>
                  <option value={1}>Java-async-profiler</option>
                  <option value={3}>eBPF-offCPU</option>
                </select>
              </label>
              <label style={{ fontSize: 13 }}>
                Target Agent IP
                <input value={targetIp} onChange={e => setTargetIp(e.target.value)}
                  placeholder="e.g. 10.0.0.1"
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
              </label>
              <label style={{ fontSize: 13 }}>
                Target PID
                <input type="number" value={pid} onChange={e => setPid(Number(e.target.value))}
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
              </label>
              <label style={{ fontSize: 13 }}>
                Sampling Frequency (Hz)
                <input type="number" value={hz} onChange={e => setHz(Number(e.target.value))}
                  placeholder="1 (low-freq for continuous)"
                  style={{ width: '100%', padding: 6, marginTop: 2, border: '1px solid #d9d9d9', borderRadius: 4, boxSizing: 'border-box' }} />
              </label>
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
