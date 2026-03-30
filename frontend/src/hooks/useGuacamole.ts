import { useRef, useState, useCallback } from 'react';
import { serialize, parse } from '../utils/guac';
import type { CanvasHandle } from '../components/CanvasViewer';

export type Protocol = 'rdp' | 'ssh' | 'vnc' | 'web';

export interface ConnectParams {
  wsUrl:    string;
  protocol: Protocol;
  // rdp / ssh / vnc
  host:     string;
  port:     string;
  username: string;
  password: string;
  // web 전용 — headless 브라우저로 열 URL
  url?:     string;
}

export type ConnectionStatus = 'idle' | 'connecting' | 'connected' | 'error' | 'disconnected';

export interface GuacState {
  status: ConnectionStatus;
  error:  string | null;
  log:    string[];
}

function base64ToBytes(b64: string): Uint8Array {
  const binary = atob(b64);
  const bytes  = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

// Guacamole img/blob/end 스트림 버퍼
interface ImageStream {
  b64Parts: string[];   // blob 청크 누적
  x:        number;
  y:        number;
  mimetype: string;
}

/**
 * @param onTerminalData SSH blob 수신 시 호출 — React state를 거치지 않고
 *                        xterm.js에 직접 write하기 위해 ref 콜백으로 전달
 * @param canvasRef      RDP / VNC / Web 렌더링 대상 캔버스 ref
 */
export function useGuacamole(
  onTerminalData?: (data: Uint8Array) => void,
  canvasRef?: React.RefObject<CanvasHandle | null>,
) {
  const wsRef             = useRef<WebSocket | null>(null);
  const onTerminalDataRef = useRef(onTerminalData);
  onTerminalDataRef.current = onTerminalData;

  const protocolRef   = useRef<Protocol>('ssh');
  // stream_id → ImageStream: img/blob/end 청크 누적
  const streamMapRef  = useRef<Map<string, ImageStream>>(new Map());

  const [state, setState] = useState<GuacState>({
    status: 'idle',
    error:  null,
    log:    [],
  });

  const addLog = useCallback((msg: string) => {
    setState(s => ({ ...s, log: [...s.log.slice(-300), msg] }));
  }, []);

  // ── connect instruction 생성 ──────────────────────────────────────────────

  const buildConnectInstruction = (params: ConnectParams): string => {
    switch (params.protocol) {
      case 'ssh':
        return serialize('connect', 'ssh', params.host, params.port,
                         params.username, params.password);
      case 'rdp':
        return serialize('connect', 'rdp', params.host, params.port,
                         params.username, params.password);
      case 'vnc':
        return serialize('connect', 'vnc', params.host, params.port,
                         params.password);
      case 'web': {
        // 캔버스 크기를 백엔드에 전달해 Chrome 뷰포트를 맞춘다.
        // 캔버스가 아직 마운트되지 않았으면 기본값 1280×800 사용.
        const { width, height } = canvasRef?.current?.getSize() ?? { width: 1280, height: 800 };
        return serialize('connect', 'web',
                         params.url ?? 'about:blank',
                         String(width), String(height));
      }
    }
  };

  // ── WebSocket 연결 ────────────────────────────────────────────────────────

  const connect = useCallback((params: ConnectParams) => {
    setState({ status: 'connecting', error: null, log: [] });
    protocolRef.current  = params.protocol;
    streamMapRef.current = new Map();

    const ws = new WebSocket(params.wsUrl);
    wsRef.current = ws;

    ws.onopen = () => {
      const instr = buildConnectInstruction(params);
      ws.send(instr);
      setState(s => ({ ...s, status: 'connected' }));
      addLog(`→ ${instr}`);
    };

    ws.onmessage = (event: MessageEvent<string>) => {
      const instrs = parse(event.data);
      for (const instr of instrs) {
        handleInstruction(instr, ws);
        addLog(`← [${instr.opcode}] ${instr.args[0] ?? ''}`);
      }
    };

    ws.onerror = () => {
      if (wsRef.current !== ws) return;
      setState(s => ({ ...s, status: 'error', error: '서버 연결 실패' }));
    };

    ws.onclose = () => {
      if (wsRef.current !== ws) return;
      setState(s => ({
        ...s,
        status: s.status === 'error' ? 'error' : 'disconnected',
      }));
      addLog('── 연결 종료 ──');
    };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [addLog]);

  // ── Guacamole 명령어 처리 ─────────────────────────────────────────────────

  const handleInstruction = (
    instr: { opcode: string; args: string[] },
    ws: WebSocket,
  ) => {
    const proto = protocolRef.current;

    switch (instr.opcode) {

      // size: 캔버스 크기 초기화 (백엔드가 연결 후 뷰포트 크기를 알려줌)
      case 'size': {
        const w = parseInt(instr.args[0], 10);
        const h = parseInt(instr.args[1], 10);
        if (!isNaN(w) && !isNaN(h)) canvasRef?.current?.resize(w, h);
        break;
      }

      // img: 새 이미지 스트림 시작 — stream_id / compositing / layer / mimetype / x / y
      case 'img': {
        const streamId  = instr.args[0];
        const mimetype  = instr.args[3] ?? 'image/jpeg';
        const x         = parseInt(instr.args[4], 10) || 0;
        const y         = parseInt(instr.args[5], 10) || 0;
        streamMapRef.current.set(streamId, { b64Parts: [], x, y, mimetype });
        break;
      }

      // blob: 스트림 데이터 청크 누적
      case 'blob': {
        const streamId = instr.args[0];
        const chunk    = instr.args[1];
        const stream   = streamMapRef.current.get(streamId);
        if (stream && chunk) stream.b64Parts.push(chunk);
        break;
      }

      // end: 스트림 종료
      //   - 이미지 스트림이 있으면 → canvas에 그린다
      //   - SSH 프로토콜이고 이미지 스트림이 없으면 → 세션 종료 신호 → ws.close()
      case 'end': {
        const streamId = instr.args[0];
        const stream   = streamMapRef.current.get(streamId);

        if (stream) {
          // 청크를 합쳐 data URL 생성 후 canvas에 그린다
          const b64     = stream.b64Parts.join('');
          const dataUrl = `data:${stream.mimetype};base64,${b64}`;
          canvasRef?.current?.drawImage(stream.x, stream.y, dataUrl);
          streamMapRef.current.delete(streamId);
        } else if (proto === 'ssh') {
          // SSH 세션 종료
          ws.close();
        }
        break;
      }

      // blob (SSH 터미널): SSH의 blob은 이미지가 아닌 터미널 바이트 출력
      //   img/end 페어가 없는 blob = SSH 터미널 출력으로 간주
      default:
        if (instr.opcode === 'blob' && proto === 'ssh') {
          const data = instr.args[1];
          if (data) onTerminalDataRef.current?.(base64ToBytes(data));
        }
        break;
    }
  };

  // ── 연결 해제 ─────────────────────────────────────────────────────────────

  const disconnect = useCallback(() => {
    wsRef.current?.close();
    wsRef.current = null;
  }, []);

  // ── 입력 전송 ─────────────────────────────────────────────────────────────

  const sendKey = useCallback((data: string) => {
    if (wsRef.current?.readyState === WebSocket.OPEN)
      wsRef.current.send(serialize('key', data));
  }, []);

  /**
   * RDP / VNC / Web 마우스 이벤트 전송
   *
   * Guacamole mouse instruction: mouse <x> <y> <buttonMask>
   * buttonMask 비트:
   *   bit 0 (1):  왼쪽 버튼
   *   bit 1 (2):  가운데 버튼
   *   bit 2 (4):  오른쪽 버튼
   *   bit 3 (8):  스크롤 위
   *   bit 4 (16): 스크롤 아래
   */
  const sendMouse = useCallback((x: number, y: number, buttonMask: number) => {
    if (wsRef.current?.readyState === WebSocket.OPEN)
      wsRef.current.send(serialize('mouse', String(x), String(y), String(buttonMask)));
  }, []);

  return { state, connect, disconnect, sendKey, sendMouse };
}
