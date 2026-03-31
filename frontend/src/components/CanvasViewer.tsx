import { forwardRef, useImperativeHandle, useRef } from 'react';

export interface CanvasHandle {
  /** 백엔드 size 명령어 수신 시 캔버스 크기 설정 */
  resize: (width: number, height: number) => void;
  /** img/blob/end 수신 완료 후 JPEG data URL을 캔버스에 그린다 */
  drawImage: (x: number, y: number, dataUrl: string) => void;
  /** 현재 캔버스 크기 반환 — connect 시 백엔드에 뷰포트 크기 전달 용도 */
  getSize: () => { width: number; height: number };
}

interface Props {
  /** 마우스 버튼 변화 시 호출: x, y, Guacamole buttonMask */
  onMouse?: (x: number, y: number, buttonMask: number) => void;
}

const CanvasViewer = forwardRef<CanvasHandle, Props>(({ onMouse }, ref) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const buttonMaskRef = useRef(0);

  useImperativeHandle(ref, () => ({
    resize(width, height) {
      const el = canvasRef.current;
      if (!el) return;
      el.width  = width;
      el.height = height;
    },
    drawImage(x, y, dataUrl) {
      const el  = canvasRef.current;
      const ctx = el?.getContext('2d');
      if (!ctx) return;
      const img = new Image();
      img.onload = () => ctx.drawImage(img, x, y);
      img.src = dataUrl;
    },
    getSize() {
      const el = canvasRef.current;
      return { width: el?.width ?? 1280, height: el?.height ?? 800 };
    },
  }));

  // ── 마우스 이벤트 → Guacamole buttonMask 변환 ─────────────────────────────
  //
  // Guacamole buttonMask 비트 정의:
  //   bit 0 (1):  왼쪽 버튼
  //   bit 1 (2):  가운데 버튼
  //   bit 2 (4):  오른쪽 버튼
  //   bit 3 (8):  스크롤 위
  //   bit 4 (16): 스크롤 아래
  //
  // React MouseEvent.button: 0=left, 1=middle, 2=right

  const getXY = (e: React.MouseEvent) => {
    const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
    return {
      x: Math.round(e.clientX - rect.left),
      y: Math.round(e.clientY - rect.top),
    };
  };

  const handleMouseMove = (e: React.MouseEvent) => {
    if (!onMouse) return;
    const { x, y } = getXY(e);
    onMouse(x, y, buttonMaskRef.current);
  };

  const handleMouseDown = (e: React.MouseEvent) => {
    if (!onMouse) return;
    const bit = 1 << e.button;  // button 0→1, 1→2, 2→4
    buttonMaskRef.current |= bit;
    const { x, y } = getXY(e);
    onMouse(x, y, buttonMaskRef.current);
  };

  const handleMouseUp = (e: React.MouseEvent) => {
    if (!onMouse) return;
    const bit = 1 << e.button;
    buttonMaskRef.current &= ~bit;
    const { x, y } = getXY(e);
    onMouse(x, y, buttonMaskRef.current);
  };

  const handleWheel = (e: React.WheelEvent) => {
    if (!onMouse) return;
    const { x, y } = getXY(e as unknown as React.MouseEvent);
    // 스크롤 위/아래는 순간적 비트 → press 후 즉시 release
    const bit = e.deltaY < 0 ? 8 : 16;
    onMouse(x, y, buttonMaskRef.current | bit);
    onMouse(x, y, buttonMaskRef.current);
  };

  const handleContextMenu = (e: React.MouseEvent) => e.preventDefault();

  return (
    <canvas
      ref={canvasRef}
      style={{ display: 'block', maxWidth: '100%', cursor: 'default' }}
      onMouseMove={handleMouseMove}
      onMouseDown={handleMouseDown}
      onMouseUp={handleMouseUp}
      onWheel={handleWheel}
      onContextMenu={handleContextMenu}
    />
  );
});

CanvasViewer.displayName = 'CanvasViewer';
export default CanvasViewer;
