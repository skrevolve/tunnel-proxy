#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <utility>

namespace proxy {

// ── 프로토콜 상수 ───────────────────────────────────────────────────────────

/**
 * @file tunnel_protocol.h
 * @brief Phase 6 — 에이전트 ↔ 서버 간 바이너리 터널 프로토콜
 *
 * ── 왜 별도 프로토콜이 필요한가 ──────────────────────────────────────────────
 *
 *   Phase 1~5의 프록시들은 클라이언트 → 서버 방향의 단순 포워딩이었다.
 *   리버스 터널은 구조가 다르다:
 *     에이전트(NAT 뒤)가 서버에 먼저 역방향 연결을 수립하고,
 *     서버는 이 하나의 TCP 연결 위에서 여러 세션을 멀티플렉싱한다.
 *
 *   하나의 TCP 연결에서 여러 세션의 데이터가 섞여 오므로
 *   "이 데이터가 어느 세션 것인가"를 구분할 프레임 포맷이 필요하다.
 *
 * ── 와이어 포맷 (big-endian, 16바이트 고정 헤더) ──────────────────────────────
 *
 *   0               1               2               3
 *   0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                    magic (0x544E4C50 = "TNLP")                |  [0-3]
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |     type      |     flags     |           reserved            |  [4-7]
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                          session_id                           |  [8-11]
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                            length                             |  [12-15]
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                    payload (length bytes)                     |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *   magic:      항상 TUNNEL_MAGIC. 수신 시 검증하여 프레임 동기화 보장.
 *               연결 도중 잘못된 데이터가 들어오면 즉시 감지 가능.
 *   type:       메시지 종류 (TunnelMsgType 참고)
 *   flags:      SYN/FIN/ACK 비트 — TCP의 플래그와 유사한 역할
 *   reserved:   현재 미사용 (0으로 설정). 향후 버전 필드 등으로 확장 예정.
 *   session_id: 0 = 컨트롤 채널 (HELLO, HEARTBEAT 등 전역 메시지)
 *               1~UINT32_MAX = 터널 세션. 서버가 OPEN 시 발급.
 *   length:     payload 크기 (0이면 payload 없음). 최대 64KB.
 *
 * ── 세션 생명주기 ─────────────────────────────────────────────────────────
 *
 *   1. 에이전트가 서버에 TCP 연결 후 HELLO 송신 (agent_id 포함)
 *   2. 서버가 HELLO_ACK 반환
 *   3. 외부 클라이언트가 서버에 접속 → 서버가 에이전트에 OPEN(session_id, target) 송신
 *   4. 에이전트가 내부 서버에 TCP 연결 후 OPEN_ACK 반환
 *   5. 양방향 DATA 교환 (session_id로 다중 세션 구분)
 *   6. 한쪽이 CLOSE 송신 → 세션 종료
 *   7. 에이전트는 주기적으로 HEARTBEAT 송신 → 서버가 HEARTBEAT_ACK 반환
 */

/// 프레임 식별자: "TNLP" (Tunnel Proxy)
static constexpr uint32_t TUNNEL_MAGIC       = 0x544E4C50;

/// 고정 헤더 크기 (bytes)
static constexpr size_t   TUNNEL_HEADER_SIZE = 16;

/// payload 최대 크기 (64KB). 초과 시 parse_header()가 예외를 던진다.
static constexpr uint32_t TUNNEL_MAX_PAYLOAD = 65536;

// ── 메시지 타입 ─────────────────────────────────────────────────────────────

enum class TunnelMsgType : uint8_t {
    HELLO         = 0x01,  // Agent → Server: 에이전트 등록 요청
    HELLO_ACK     = 0x02,  // Server → Agent: 등록 승인
    OPEN          = 0x03,  // Server → Agent: 새 터널 세션 시작 요청
    OPEN_ACK      = 0x04,  // Agent → Server: 세션 수락 확인
    DATA          = 0x05,  // 양방향: 실제 데이터 전송
    CLOSE         = 0x06,  // 양방향: 세션 종료 통보
    HEARTBEAT     = 0x07,  // Agent → Server: 연결 유지 ping
    HEARTBEAT_ACK = 0x08,  // Server → Agent: 연결 유지 pong
    ERROR         = 0x09,  // 양방향: 에러 상태 전달
};

// ── 프레임 플래그 ───────────────────────────────────────────────────────────

namespace TunnelFlags {
    static constexpr uint8_t NONE = 0x00;
    static constexpr uint8_t FIN  = 0x01;  // 세션/연결 종료 의사
    static constexpr uint8_t SYN  = 0x02;  // 새 세션/연결 시작
    static constexpr uint8_t ACK  = 0x04;  // 이전 메시지 승인
} // namespace TunnelFlags

// ── 프레임 구조체 ───────────────────────────────────────────────────────────

/**
 * TunnelFrame — 프로토콜 프레임의 C++ 표현
 *
 * 직렬화 시 serialize()로 네트워크 바이트 순서(big-endian) 버퍼로 변환.
 * 역직렬화 시 parse_header() + parse_frame()으로 복원.
 */
struct TunnelFrame {
    uint32_t      magic;       // 항상 TUNNEL_MAGIC
    TunnelMsgType type;        // 메시지 종류
    uint8_t       flags;       // TunnelFlags 비트 조합
    uint16_t      reserved;    // 0으로 설정
    uint32_t      session_id;  // 0=컨트롤, 1+= 터널 세션
    uint32_t      length;      // payload 크기
    std::vector<uint8_t> payload;

    TunnelFrame()
        : magic(TUNNEL_MAGIC), type(TunnelMsgType::DATA),
          flags(TunnelFlags::NONE), reserved(0),
          session_id(0), length(0) {}

    TunnelFrame(TunnelMsgType t, uint32_t sid,
                std::vector<uint8_t> pl = {},
                uint8_t f = TunnelFlags::NONE)
        : magic(TUNNEL_MAGIC), type(t), flags(f), reserved(0),
          session_id(sid), length(static_cast<uint32_t>(pl.size())),
          payload(std::move(pl)) {}
};

// ── 직렬화 / 역직렬화 ───────────────────────────────────────────────────────

/**
 * TunnelFrame → 네트워크 바이트 버퍼 (big-endian)
 *
 * 반환값: 헤더 16바이트 + payload 연속 버퍼.
 * 호출 전 frame.length == frame.payload.size()가 보장되어야 한다.
 *
 * 왜 big-endian(네트워크 바이트 순서)인가:
 *   에이전트와 서버가 x86(little-endian) / ARM(either) 등 다른 아키텍처에서
 *   실행될 수 있다. 프로토콜이 바이트 순서를 고정하지 않으면 플랫폼 조합에 따라
 *   필드 값이 깨진다. htonl/htons로 항상 big-endian으로 변환해 플랫폼 독립성 보장.
 */
std::vector<uint8_t> serialize(const TunnelFrame& frame);

/**
 * 헤더 16바이트 파싱 → TunnelFrame (payload 제외)
 *
 * @param buf      읽기 버퍼 포인터
 * @param buf_len  버퍼 크기 (TUNNEL_HEADER_SIZE 이상이어야 함)
 * @return 헤더만 채워진 TunnelFrame. payload는 빈 벡터.
 *
 * 왜 헤더만 먼저 파싱하는가:
 *   TCP는 스트림 기반이라 데이터가 여러 번에 나뉘어 도착할 수 있다.
 *   먼저 16바이트를 읽어 length를 파악하고,
 *   그 다음 length 바이트를 읽어 payload를 채우는 2단계 읽기가 안전하다.
 *   한 번에 length만큼 버퍼를 할당하면 잘못된 length로 인한 OOM을 방지하기 위해
 *   magic 검증과 length 상한 검사를 이 함수에서 먼저 수행한다.
 *
 * 예외:
 *   - buf_len < TUNNEL_HEADER_SIZE   → "buffer too small for header"
 *   - magic != TUNNEL_MAGIC          → "invalid magic bytes"
 *   - length > TUNNEL_MAX_PAYLOAD    → "payload length exceeds maximum"
 */
TunnelFrame parse_header(const uint8_t* buf, size_t buf_len);

/**
 * 헤더 + payload → 완전한 TunnelFrame
 *
 * @param header_buf  헤더 버퍼 (TUNNEL_HEADER_SIZE 바이트)
 * @param payload_buf payload 버퍼
 * @param payload_len payload 버퍼 크기 (frame.length 이상이어야 함)
 *
 * 예외: parse_header()의 예외 + payload_len < frame.length
 */
TunnelFrame parse_frame(const uint8_t* header_buf,
                        const uint8_t* payload_buf, uint32_t payload_len);

// ── 편의 생성 함수 ──────────────────────────────────────────────────────────

/**
 * HELLO 프레임 (에이전트 등록)
 *
 * payload 포맷: [1바이트 길이][agent_id 문자열]
 *
 * 왜 length-prefixed인가:
 *   null-terminated 방식은 agent_id에 null이 포함되면 파싱이 잘못된다.
 *   length-prefixed는 임의의 바이너리 데이터도 안전하게 포함 가능하고,
 *   수신 측에서 버퍼 오버런 없이 정확한 크기를 읽을 수 있다.
 *
 * @param agent_id 에이전트 고유 식별자 (최대 255자)
 */
TunnelFrame make_hello(const std::string& agent_id);

/** HELLO_ACK 프레임 */
TunnelFrame make_hello_ack();

/**
 * OPEN 프레임 (서버 → 에이전트: 새 세션 열기 요청)
 *
 * payload 포맷: [4바이트 IPv4 (network order)][2바이트 포트 (network order)]
 *
 * 에이전트는 이 프레임을 받으면 target_ip:target_port에 TCP 연결을 수립하고
 * OPEN_ACK로 응답한다. 이후 해당 session_id로 DATA를 교환한다.
 *
 * @param session_id  서버가 발급한 세션 식별자 (1 이상)
 * @param target_ip   에이전트가 연결할 내부 서버 IPv4 주소
 * @param target_port 에이전트가 연결할 내부 서버 포트
 */
TunnelFrame make_open(uint32_t session_id,
                      const std::string& target_ip, uint16_t target_port);

/** OPEN_ACK 프레임 (에이전트 → 서버: 세션 수락 확인) */
TunnelFrame make_open_ack(uint32_t session_id);

/**
 * DATA 프레임 (양방향 데이터 전송)
 *
 * @param session_id 어느 세션의 데이터인지 (0은 사용 불가)
 * @param data       전달할 원시 바이트 데이터
 */
TunnelFrame make_data(uint32_t session_id, std::vector<uint8_t> data);

/** CLOSE 프레임 — session_id 세션을 종료한다. */
TunnelFrame make_close(uint32_t session_id);

/** HEARTBEAT 프레임 — 에이전트가 주기적으로 서버에 전송 */
TunnelFrame make_heartbeat();

/** HEARTBEAT_ACK 프레임 — 서버가 HEARTBEAT에 응답 */
TunnelFrame make_heartbeat_ack();

// ── payload 파서 ────────────────────────────────────────────────────────────

/**
 * OPEN payload → (target_ip, target_port)
 *
 * @param payload make_open()이 생성한 6바이트 payload
 * @return {IPv4 문자열, 포트 번호}
 *
 * 예외: payload.size() < 6 → "OPEN payload too short"
 */
std::pair<std::string, uint16_t> parse_open_payload(
    const std::vector<uint8_t>& payload);

/**
 * HELLO payload → agent_id 문자열
 *
 * @param payload make_hello()가 생성한 payload
 * @return agent_id 문자열
 *
 * 예외: payload 비어있거나 길이 필드 불일치 → 런타임 예외
 */
std::string parse_hello_payload(const std::vector<uint8_t>& payload);

} // namespace proxy
