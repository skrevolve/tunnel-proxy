import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import type { Protocol, ConnectParams } from '../hooks/useGuacamole';

const DEFAULT_PORTS: Partial<Record<Protocol, string>> = {
  rdp: '3389',
  ssh: '22',
  vnc: '5900',
};

const PROTOCOL_LABELS: Record<Protocol, string> = {
  rdp: 'RDP',
  ssh: 'SSH',
  vnc: 'VNC',
  web: 'WEB',
};

export default function ConnectPage() {
  const navigate = useNavigate();
  const [form, setForm] = useState<ConnectParams>({
    wsUrl:    'ws://localhost:8765',
    protocol: 'ssh',
    host:     'localhost',
    port:     '22',
    username: '',
    password: '',
    url:      'https://example.com',
  });

  const handleProtocol = (p: Protocol) => {
    setForm(f => ({
      ...f,
      protocol: p,
      port: DEFAULT_PORTS[p] ?? '',
    }));
  };

  const handleSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    navigate('/session', { state: form });
  };

  const isWeb = form.protocol === 'web';

  return (
    <div className="page center">
      <div className="card">
        <h1>tunnel-proxy</h1>
        <p className="subtitle">원격 접속 게이트웨이</p>

        <form onSubmit={handleSubmit} className="form">
          <div className="field">
            <label>게이트웨이 WebSocket URL</label>
            <input
              value={form.wsUrl}
              onChange={e => setForm(f => ({ ...f, wsUrl: e.target.value }))}
              placeholder="ws://localhost:8765"
              required
            />
          </div>

          <div className="field">
            <label>프로토콜</label>
            <div className="radio-group">
              {(['rdp', 'ssh', 'vnc', 'web'] as Protocol[]).map(p => (
                <label
                  key={p}
                  className={`radio ${form.protocol === p ? 'active' : ''}`}
                >
                  <input
                    type="radio"
                    name="protocol"
                    value={p}
                    checked={form.protocol === p}
                    onChange={() => handleProtocol(p)}
                  />
                  {PROTOCOL_LABELS[p]}
                </label>
              ))}
            </div>
          </div>

          {isWeb ? (
            /* web 프로토콜: host/port 대신 URL 입력 */
            <div className="field">
              <label>URL</label>
              <input
                value={form.url ?? ''}
                onChange={e => setForm(f => ({ ...f, url: e.target.value }))}
                placeholder="https://example.com"
                required
              />
            </div>
          ) : (
            /* rdp / ssh / vnc: host:port 입력 */
            <div className="row">
              <div className="field flex-1">
                <label>호스트</label>
                <input
                  value={form.host}
                  onChange={e => setForm(f => ({ ...f, host: e.target.value }))}
                  placeholder="192.168.0.1"
                  required
                />
              </div>
              <div className="field port-field">
                <label>포트</label>
                <input
                  value={form.port}
                  onChange={e => setForm(f => ({ ...f, port: e.target.value }))}
                  required
                />
              </div>
            </div>
          )}

          {/* 사용자명: rdp / ssh만 표시 */}
          {(form.protocol === 'rdp' || form.protocol === 'ssh') && (
            <div className="field">
              <label>사용자명</label>
              <input
                value={form.username}
                onChange={e => setForm(f => ({ ...f, username: e.target.value }))}
                autoComplete="username"
              />
            </div>
          )}

          {/* 비밀번호: web 제외 */}
          {!isWeb && (
            <div className="field">
              <label>비밀번호</label>
              <input
                type="password"
                value={form.password}
                onChange={e => setForm(f => ({ ...f, password: e.target.value }))}
                autoComplete="current-password"
              />
            </div>
          )}

          <button type="submit" className="btn-primary">
            연결
          </button>
        </form>
      </div>
    </div>
  );
}
