#pragma once

#include <string>
#include <vector>
#include <queue>
#include <stdexcept>

namespace proxy {

/**
 * @file guac_parser.h
 * @brief Phase 8-A — Apache Guacamole 텍스트 프로토콜 파서
 *
 * ── Guacamole 프로토콜이란 ───────────────────────────────────────────────
 *
 *   Apache Guacamole는 브라우저 ↔ 게이트웨이 간 통신에 자체 텍스트 프로토콜을
 *   사용한다. 이 프로토콜로 RDP/VNC/SSH 화면을 브라우저에 스트리밍하거나
 *   키보드·마우스 입력을 서버로 전달한다.
 *
 * ── 와이어 포맷 ──────────────────────────────────────────────────────────
 *
 *   instruction  ::= element (',' element)* ';'
 *   element      ::= LENGTH '.' VALUE
 *   LENGTH       ::= <10진수 정수, VALUE의 바이트 수>
 *   VALUE        ::= <임의의 바이트 시퀀스, 길이 = LENGTH>
 *
 *   예시:
 *     "4.draw,1.0,3.100,3.200;"
 *      └─ opcode="draw", args=["0","100","200"]
 *
 *     "4.sync,13.1000000000000;"
 *      └─ opcode="sync", args=["1000000000000"]
 *
 *   첫 번째 element는 opcode(명령어 이름), 이후는 arguments.
 *
 * ── 왜 length-prefixed인가 ───────────────────────────────────────────────
 *
 *   CSV 방식(구분자 기반)은 VALUE 안에 구분자가 포함되면 이스케이프가 필요하다.
 *   length-prefixed는 VALUE의 정확한 길이를 미리 알고 읽으므로
 *   임의의 바이너리 데이터(이미지 청크 등)도 안전하게 포함 가능하다.
 *
 * ── 스트리밍 파싱 지원 ────────────────────────────────────────────────────
 *
 *   WebSocket은 패킷 경계와 Guacamole 명령어 경계가 일치하지 않을 수 있다.
 *   (TCP와 동일한 스트림 특성)
 *   GuacParser::feed()로 데이터를 점진적으로 공급하고,
 *   has_instruction() / next_instruction()으로 완성된 명령어를 꺼낸다.
 *
 * ── 파서 상태 머신 ────────────────────────────────────────────────────────
 *
 *   READING_LENGTH  → 길이 숫자 누적 (문자가 '.'이 되면 READING_ELEMENT로 전환)
 *   READING_ELEMENT → current_len_ 바이트 읽기 (완료되면 READING_SEP으로 전환)
 *   READING_SEP     → ',' (다음 element) 또는 ';' (명령어 완료) 기대
 */

// ── 명령어 구조체 ─────────────────────────────────────────────────────────

/**
 * GuacInstruction — Guacamole 명령어 하나를 나타내는 C++ 구조체
 *
 * opcode: 명령어 이름 (예: "draw", "sync", "key", "mouse")
 * args:   인수 목록 (빈 벡터 가능)
 */
struct GuacInstruction {
    std::string              opcode;
    std::vector<std::string> args;

    GuacInstruction() = default;
    GuacInstruction(std::string op, std::vector<std::string> a = {})
        : opcode(std::move(op)), args(std::move(a)) {}

    bool operator==(const GuacInstruction& other) const {
        return opcode == other.opcode && args == other.args;
    }
};

// ── 파서 클래스 ────────────────────────────────────────────────────────────

/**
 * GuacParser — 스트리밍 Guacamole 명령어 파서
 *
 * 사용 예:
 *   GuacParser parser;
 *   parser.feed("4.draw,1.0;4.sy");   // 부분 데이터 공급
 *   parser.feed("nc,1.0;");           // 나머지 공급
 *
 *   while (parser.has_instruction()) {
 *       auto instr = parser.next_instruction();
 *       // opcode = "draw", args = ["0"]
 *       // opcode = "sync", args = ["0"]
 *   }
 */
class GuacParser {
public:
    GuacParser();

    /**
     * 데이터를 파서 버퍼에 공급한다.
     *
     * @param data 수신한 원시 데이터
     * @param len  데이터 크기
     *
     * 예외: 파싱 오류 (잘못된 길이, 예상치 못한 구분자 등) → std::runtime_error
     */
    void feed(const char* data, size_t len);
    void feed(const std::string& data);

    /**
     * 완성된 명령어가 큐에 존재하는지 확인한다.
     *
     * @return 꺼낼 수 있는 명령어가 하나 이상 있으면 true
     */
    bool has_instruction() const;

    /**
     * 완성된 명령어를 큐에서 꺼낸다.
     *
     * @return 가장 오래된 완성 명령어
     * @throws std::runtime_error has_instruction()이 false인 경우
     */
    GuacInstruction next_instruction();

    /**
     * 파서 내부 상태를 초기화한다.
     * 연결 재시작 등 스트림을 처음부터 다시 읽어야 할 때 호출.
     */
    void reset();

    // ── 직렬화 ─────────────────────────────────────────────────────────────

    /**
     * GuacInstruction → 와이어 포맷 문자열
     *
     * 예: {"draw", {"0","100","200"}} → "4.draw,1.0,3.100,3.200;"
     *
     * @param instr 직렬화할 명령어
     * @return 와이어 포맷 문자열 (';'로 종료)
     *
     * 예외: opcode가 비어 있으면 std::invalid_argument
     */
    static std::string serialize(const GuacInstruction& instr);

private:
    // ── 파서 상태 머신 ──────────────────────────────────────────────────────

    enum class State {
        READING_LENGTH,   // 길이 숫자 누적 중 ('.'가 나올 때까지)
        READING_ELEMENT,  // current_len_ 바이트를 element 버퍼로 읽는 중
        READING_SEP,      // ',' 또는 ';' 기대
    };

    State                    state_;
    std::string              len_buf_;      // 길이 숫자 누적 버퍼
    std::string              elem_buf_;     // 현재 element 내용 누적 버퍼
    size_t                   current_len_;  // 읽어야 할 element 바이트 수
    GuacInstruction          current_;      // 조립 중인 명령어
    std::queue<GuacInstruction> ready_;     // 완성된 명령어 큐

    // 한 바이트를 처리한다. (feed()의 내부 루프에서 호출)
    void process_byte(char c);
};

} // namespace proxy
