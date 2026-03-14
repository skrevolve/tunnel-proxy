import { useRef, useState, useCallback } from 'react';
import { serialize, parse } from '../utils/guac';

export type Protocol = 'rdp' | 'ssh' | 'vnc';

export interface ConnectParams {
  wsUrl: string;
  protocol: Protocol;
  host: string;
  port: string;
  username: string;
  password: string;
}

export type ConnectionStatus = 'idle' | 'connecting' | 'connected' | 'error' | 'disconnected';

export interface GuacState {
  status: ConnectionStatus;
  error: string | null;
  log: string[];
}

export function useGuacamole() {
  const wsRef = useRef<WebSocket | null>(null);
  const [state, setState] = useState<GuacState>({
    status: 'idle',
    error: null,
    log: [],
  });

  const addLog = useCallback((msg: string) => {
    setState(s => ({ ...s, log: [...s.log.slice(-300), msg] }));
  }, []);

  const connect = useCallback((params: ConnectParams) => {
    setState({ status: 'connecting', error: null, log: [] });

    const ws = new WebSocket(params.wsUrl);
    wsRef.current = ws;

    ws.onopen = () => {
      // connect 명령어를 첫 번째 프레임으로 즉시 전송
      const instr =
        params.protocol === 'vnc'
          ? serialize('connect', params.protocol, params.host, params.port, params.password)
          : serialize('connect', params.protocol, params.host, params.port, params.username, params.password);

      ws.send(instr);
      setState(s => ({ ...s, status: 'connected' }));
      addLog(`→ ${instr}`);
    };

    ws.onmessage = (event: MessageEvent<string>) => {
      const instrs = parse(event.data);
      for (const instr of instrs) {
        addLog(`← [${instr.opcode}] ${instr.args.join(' | ')}`);
      }
    };

    ws.onerror = () => {
      setState(s => ({ ...s, status: 'error', error: '서버 연결 실패' }));
    };

    ws.onclose = () => {
      setState(s => ({
        ...s,
        status: s.status === 'error' ? 'error' : 'disconnected',
      }));
      addLog('── 연결 종료 ──');
    };
  }, [addLog]);

  const disconnect = useCallback(() => {
    wsRef.current?.close();
    wsRef.current = null;
  }, []);

  const sendKey = useCallback((key: string) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(serialize('key', key));
    }
  }, []);

  return { state, connect, disconnect, sendKey };
}
