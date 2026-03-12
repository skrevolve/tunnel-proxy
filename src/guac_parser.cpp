#include "core/guac_parser.h"
#include <stdexcept>
#include <string>

namespace proxy {

// ── 생성자 / reset ────────────────────────────────────────────────────────

GuacParser::GuacParser()
    : state_(State::READING_LENGTH), current_len_(0)
{}

void GuacParser::reset() {
    state_       = State::READING_LENGTH;
    current_len_ = 0;
    len_buf_.clear();
    elem_buf_.clear();
    current_ = GuacInstruction{};
    while (!ready_.empty()) ready_.pop();
}

// ── feed ─────────────────────────────────────────────────────────────────

void GuacParser::feed(const std::string& data) {
    feed(data.data(), data.size());
}

void GuacParser::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        process_byte(data[i]);
    }
}

// ── 내부 상태 머신 ────────────────────────────────────────────────────────

/**
 * process_byte — 한 바이트를 상태 머신에 공급한다.
 *
 * 상태 전환:
 *
 *   READING_LENGTH:
 *     - '0'-'9' → len_buf_에 누적
 *     - '.'     → len_buf_를 current_len_으로 변환, READING_ELEMENT로 전환
 *                 current_len_ == 0이면 element는 빈 문자열 → 즉시 READING_SEP
 *     - 그 외   → 파싱 오류
 *
 *   READING_ELEMENT:
 *     - 아무 바이트 → elem_buf_에 누적
 *     - elem_buf_.size() == current_len_이 되면 READING_SEP으로 전환
 *
 *   READING_SEP:
 *     - ','  → element를 current_.args 또는 opcode로 저장, 다음 element 준비
 *     - ';'  → element를 저장, 명령어 완성 → ready_ 큐에 push, 새 명령어 시작
 *     - 그 외 → 파싱 오류
 */
void GuacParser::process_byte(char c) {
    switch (state_) {

    // ── 길이 숫자 읽기 ───────────────────────────────────────────────────
    case State::READING_LENGTH:
        if (c >= '0' && c <= '9') {
            len_buf_ += c;
        } else if (c == '.') {
            if (len_buf_.empty()) {
                throw std::runtime_error("guac_parser: length field is empty before '.'");
            }
            // stoul이 던지는 예외는 그대로 전파 (malformed length)
            current_len_ = static_cast<size_t>(std::stoul(len_buf_));
            len_buf_.clear();
            elem_buf_.clear();
            elem_buf_.reserve(current_len_);
            state_ = State::READING_ELEMENT;

            // 길이가 0이면 element가 빈 문자열 → READING_SEP으로 즉시 전환
            if (current_len_ == 0) {
                state_ = State::READING_SEP;
            }
        } else {
            throw std::runtime_error(
                std::string("guac_parser: unexpected char '") + c +
                "' in length field");
        }
        break;

    // ── element 내용 읽기 ────────────────────────────────────────────────
    case State::READING_ELEMENT:
        elem_buf_ += c;
        if (elem_buf_.size() == current_len_) {
            state_ = State::READING_SEP;
        }
        break;

    // ── 구분자 읽기 ──────────────────────────────────────────────────────
    case State::READING_SEP:
        if (c == ',' || c == ';') {
            // element 저장: 첫 element = opcode, 이후 = args
            if (current_.opcode.empty()) {
                current_.opcode = std::move(elem_buf_);
            } else {
                current_.args.push_back(std::move(elem_buf_));
            }
            elem_buf_.clear();

            if (c == ',') {
                // 다음 element 읽기 준비
                state_ = State::READING_LENGTH;
            } else {
                // ';' → 명령어 완성
                if (current_.opcode.empty()) {
                    throw std::runtime_error("guac_parser: instruction with empty opcode");
                }
                ready_.push(std::move(current_));
                current_ = GuacInstruction{};
                state_   = State::READING_LENGTH;
            }
        } else {
            throw std::runtime_error(
                std::string("guac_parser: expected ',' or ';' but got '") + c + "'");
        }
        break;
    }
}

// ── 완성 명령어 조회 ──────────────────────────────────────────────────────

bool GuacParser::has_instruction() const {
    return !ready_.empty();
}

GuacInstruction GuacParser::next_instruction() {
    if (ready_.empty()) {
        throw std::runtime_error("guac_parser: no instruction available");
    }
    GuacInstruction instr = std::move(ready_.front());
    ready_.pop();
    return instr;
}

// ── 직렬화 ────────────────────────────────────────────────────────────────

/**
 * serialize — GuacInstruction → 와이어 포맷
 *
 * 포맷: LENGTH.VALUE[,LENGTH.VALUE]...;
 *
 * 예) {"draw", {"0","100","200"}} → "4.draw,1.0,3.100,3.200;"
 *
 * 왜 opcode를 별도 처리하지 않는가:
 *   opcode와 args는 와이어 포맷 상으로 동일한 element다.
 *   opcode를 첫 번째 element로 포함시켜 동일 로직으로 직렬화하면
 *   코드 중복 없이 모든 element를 일관되게 처리할 수 있다.
 */
std::string GuacParser::serialize(const GuacInstruction& instr) {
    if (instr.opcode.empty()) {
        throw std::invalid_argument("guac_parser: opcode must not be empty");
    }

    std::string result;

    // opcode
    result += std::to_string(instr.opcode.size());
    result += '.';
    result += instr.opcode;

    // args
    for (const auto& arg : instr.args) {
        result += ',';
        result += std::to_string(arg.size());
        result += '.';
        result += arg;
    }

    result += ';';
    return result;
}

} // namespace proxy
