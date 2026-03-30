import { useEffect, useRef, useState } from 'react';
import { useLocation, useNavigate } from 'react-router-dom';
import { useGuacamole } from '../hooks/useGuacamole';
import TerminalViewer, { type TerminalHandle } from '../components/TerminalViewer';
import CanvasViewer, { type CanvasHandle } from '../components/CanvasViewer';
import type { ConnectParams } from '../hooks/useGuacamole';

const STATUS_COLOR: Record<string, string> = {
  idle:         '#718096',
  connecting:   '#f6ad55',
  connected:    '#68d391',
  error:        '#fc8181',
  disconnected: '#718096',
};

export default function SessionPage() {
  const location = useLocation();
  const navigate = useNavigate();
  const params   = location.state as ConnectParams | null;

  const termRef   = useRef<TerminalHandle>(null);
  const canvasRef = useRef<CanvasHandle>(null);
  const [showLog, setShowLog] = useState(false);

  const { state, connect, disconnect, sendKey, sendMouse } = useGuacamole(
    (data) => termRef.current?.write(data),
    canvasRef,
  );

  useEffect(() => {
    if (!params) { navigate('/'); return; }
    connect(params);
    return () => disconnect();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const logRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (showLog && logRef.current) logRef.current.scrollTop = logRef.current.scrollHeight;
  }, [state.log, showLog]);

  const handleDisconnect = () => {
    disconnect();
    navigate('/');
  };

  const isSsh    = params?.protocol === 'ssh';
  const isCanvas = params?.protocol === 'rdp'
                || params?.protocol === 'vnc'
                || params?.protocol === 'web';

  return (
    <div className="page session">
      <header className="session-header">
        <div className="session-info">
          <span className="status-dot" style={{ background: STATUS_COLOR[state.status] }} />
          <span className="session-title">
            {params?.protocol?.toUpperCase()} —{' '}
            {params?.protocol === 'web'
              ? (params.url ?? 'about:blank')
              : `${params?.host}:${params?.port}`}
          </span>
          <span className="status-label">{state.status}</span>
        </div>
        <div className="header-actions">
          <button
            className={`btn-debug${showLog ? ' active' : ''}`}
            onClick={() => setShowLog(v => !v)}
            title="디버그 로그"
          >
            DEBUG
          </button>
          <button className="btn-danger" onClick={handleDisconnect}>
            연결 해제
          </button>
        </div>
      </header>

      <div className="session-body">
        {state.error && <div className="error-banner">{state.error}</div>}

        <div className="canvas-area" style={{ position: 'relative' }}>
          {isSsh && (
            <TerminalViewer ref={termRef} onInput={sendKey} />
          )}
          {isCanvas && (
            <CanvasViewer
              ref={canvasRef}
              onMouse={sendMouse}
            />
          )}
          {state.status === 'disconnected' && (
            <div className="session-ended-overlay">
              <div className="session-ended-box">
                <span>세션이 종료되었습니다</span>
                <button className="btn-primary" onClick={() => navigate('/')}>
                  돌아가기
                </button>
              </div>
            </div>
          )}
        </div>

        {showLog && (
          <div className="log-panel">
            <div className="log-header">프로토콜 로그</div>
            <div className="log" ref={logRef}>
              {state.log.map((line, i) => (
                <div key={i} className="log-line">{line}</div>
              ))}
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
