import { useEffect, useState } from 'react';
import { Link } from 'react-router-dom';
import { listAgents, statAgent } from '../api';

interface Agent {
  id: number;
  hostname: string;
  ip_addr: string;
  online: boolean;
  uid: string;
  version: string;
}

export default function HomePage() {
  const [agents, setAgents] = useState<Agent[]>([]);

  useEffect(() => {
    statAgent().then(res => setAgents(res.data.data ?? []))
      .catch(() => listAgents().then(res => setAgents(res.data.data ?? [])));
  }, []);

  const onlineCount = agents.filter(a => a.online).length;

  return (
    <div style={{ maxWidth: 1100, margin: '0 auto', padding: '24px 16px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 24 }}>
        <h1 style={{ margin: 0 }}>Drop Performance Profiler</h1>
        <Link to="/tasks" style={{ color: '#1890ff' }}>All Tasks →</Link>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(280px, 1fr))', gap: 16 }}>
        {agents.length === 0 && <p style={{ color: '#8c8c8c' }}>No agents registered yet.</p>}
        {agents.map(a => (
          <div key={a.id} style={{
            border: '1px solid #f0f0f0', borderRadius: 8, padding: 16,
            boxShadow: '0 1px 2px rgba(0,0,0,0.06)',
          }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
              <strong>{a.hostname}</strong>
              <span style={{
                padding: '2px 8px', borderRadius: 4, fontSize: 12,
                background: a.online ? '#f6ffed' : '#fff2f0',
                color: a.online ? '#52c41a' : '#ff4d4f',
              }}>
                {a.online ? 'Online' : 'Offline'}
              </span>
            </div>
            <div style={{ marginTop: 8, color: '#595959', fontSize: 13 }}>
              IP: {a.ip_addr}<br />
              Version: {a.version}<br />
              UID: {a.uid}
            </div>
          </div>
        ))}
      </div>

      <div style={{ marginTop: 32, padding: 16, background: '#fafafa', borderRadius: 8 }}>
        <strong>Agent Summary:</strong> {agents.length} registered, {onlineCount} online
      </div>
    </div>
  );
}
