#pragma once

#include <string>

namespace proxy {

/**
 * @file tunnel_server.h
 * @brief Phase 6-C — 터널 서버 (에이전트 연결 수신 측)
 *
 * TODO Phase 6-C (feat/tunnel-server):
 *   - 에이전트의 역방향 TCP 연결 수락
 *   - HELLO 수신 → agent_id 등록 → HELLO_ACK 송신
 *   - 외부 클라이언트 접속 시 session_id 발급 → OPEN 송신
 *   - 세션 맵 (session_id → agent fd) 관리
 *   - HEARTBEAT_ACK 응답 + 타임아웃 감지
 */

// TODO: Phase 6-C에서 구현
// class TunnelServer { ... };

} // namespace proxy
