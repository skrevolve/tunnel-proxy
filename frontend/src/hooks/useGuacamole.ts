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

function base64ToBytes(b64: string): Uint8Array {
  const binary = atob(b64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

/**
 * @param onTerminalData SSH blob 수신 시 호출 — React state를 거치지 않고
 *                        xterm.js에 직접 write하기 위해 ref 콜백으로 전달
 */
export function useGuacamole(onTerminalData?: (data: Uint8Array) => void) {
  const wsRef              = useRef<WebSocket | null>(null);
  const onTerminalDataRef  = useRef(onTerminalData);
  onTerminalDataRef.current = onTerminalData;

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
        // blob: SSH 터미널 출력 — base64 디코딩 후 xterm에 직접 전달
        if (instr.opcode === 'blob' && instr.args[1]) {
          onTerminalDataRef.current?.(base64ToBytes(instr.args[1]));
        }
        addLog(`← [${instr.opcode}] ${instr.args[0] ?? ''}`);
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

  const sendKey = useCallback((data: string) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(serialize('key', data));
    }
  }, []);

  return { state, connect, disconnect, sendKey };
}
