import { useEffect, useImperativeHandle, useRef, forwardRef } from 'react';
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';

export interface TerminalHandle {
  write: (data: Uint8Array) => void;
  fit:   () => void;
}

interface Props {
  onInput: (data: string) => void;
}

const TerminalViewer = forwardRef<TerminalHandle, Props>(({ onInput }, ref) => {
  const containerRef = useRef<HTMLDivElement>(null);
  const termRef      = useRef<Terminal | null>(null);
  const fitRef       = useRef<FitAddon | null>(null);

  useImperativeHandle(ref, () => ({
    write: (data: Uint8Array) => termRef.current?.write(data),
    fit:   () => fitRef.current?.fit(),
  }));

  useEffect(() => {
    const term = new Terminal({
      theme: {
        background:  '#0f1117',
        foreground:  '#e2e8f0',
        cursor:      '#4f8ef7',
        black:       '#1a1d27',
        brightBlack: '#4a5568',
      },
      fontFamily:  "'JetBrains Mono', 'Fira Code', Consolas, monospace",
      fontSize:    14,
      lineHeight:  1.3,
      cursorBlink: true,
      scrollback:  5000,
    });

    const fit = new FitAddon();
    term.loadAddon(fit);

    if (containerRef.current) {
      term.open(containerRef.current);
      fit.fit();
    }

    // eslint-disable-next-line react-hooks/exhaustive-deps
    term.onData(onInput);

    termRef.current = term;
    fitRef.current  = fit;

    const observer = new ResizeObserver(() => fit.fit());
    if (containerRef.current) observer.observe(containerRef.current);

    return () => {
      observer.disconnect();
      term.dispose();
    };
  // onInput은 마운트 시 1회만 등록 — deps 제외 의도적
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <div
      ref={containerRef}
      style={{ height: '100%', width: '100%', padding: '4px' }}
    />
  );
});

TerminalViewer.displayName = 'TerminalViewer';
export default TerminalViewer;
