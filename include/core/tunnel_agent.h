#pragma once

#include <string>

namespace proxy {

/**
 * @file tunnel_agent.h
 * @brief Phase 6-B — 에이전트 (NAT 뒤 클라이언트 측)
 *
 * TODO Phase 6-B (feat/tunnel-agent):
 *   - 서버 IP:포트에 TCP 연결 유지 (역방향 연결)
 *   - HELLO 송신 → HELLO_ACK 수신
 *   - OPEN 수신 → 내부 타겟에 TCP 연결 → OPEN_ACK 송신
 *   - DATA 양방향 포워딩 (session_id별 분리)
 *   - HEARTBEAT 주기적 송신
 */

// TODO: Phase 6-B에서 구현
// class TunnelAgent { ... };

} // namespace proxy
