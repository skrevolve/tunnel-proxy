import { useEffect, useRef } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { useGuacamole } from '../hooks/useGuacamole';
import TerminalViewer, { type TerminalHandle } from '../components/TerminalViewer';
import type { ConnectParams } from '../hooks/useGuacamole';

const STATUS_COLOR: Record<string, string> = {
  idle:         '#718096',
  connecting:   '#f6ad55',
  connected:    '#68d391',
  error:        '#fc8181',
  disconnected: '#718096',
};

export default function SessionPage() {
  const location  = useLocation();
  const navigate  = useNavigate();
  const params    = location.state as ConnectParams | null;
  const termRef   = useRef<TerminalHandle>(null);

  const { state, connect, disconnect, sendKey } = useGuacamole(
    (data) => termRef.current?.write(data)
  );

  useEffect(() => {
    if (!params) { navigate('/'); return; }
    connect(params);
    return () => disconnect();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // 로그 패널 자동 스크롤
  const logRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [state.log]);

  const handleDisconnect = () => {
    disconnect();
    navigate('/');
  };

  const isSsh = params?.protocol === 'ssh';

  return (
    <div className="page session">
      <header className="session-header">
        <div className="session-info">
          <span className="status-dot" style={{ background: STATUS_COLOR[state.status] }} />
          <span className="session-title">
            {params?.protocol?.toUpperCase()} — {params?.host}:{params?.port}
          </span>
          <span className="status-label">{state.status}</span>
        </div>
        <button className="btn-danger" onClick={handleDisconnect}>
          연결 해제
        </button>
      </header>

      <div className="session-body">
        {state.error && <div className="error-banner">{state.error}</div>}

        <div className="canvas-area">
          {isSsh ? (
            <TerminalViewer ref={termRef} onInput={sendKey} />
          ) : (
            <span>RDP / VNC 렌더링 구현 예정</span>
          )}
        </div>

        <div className="log-panel">
          <div className="log-header">프로토콜 로그</div>
          <div className="log" ref={logRef}>
            {state.log.map((line, i) => (
              <div key={i} className="log-line">{line}</div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}
