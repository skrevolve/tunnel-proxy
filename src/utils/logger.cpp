#include "utils/logger.h"
#include <iostream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>

// ── 전역 상태 ──────────────────────────────────────────────────────────────────
//
// Logger는 인스턴스 없이 static 메서드로만 동작한다.
// 상태(레벨, 파일 경로)는 파일 스코프 static 변수로 관리한다.
//
// static으로 선언하는 이유:
//   이 변수들은 logger.cpp 내부에서만 접근해야 한다.
//   static을 붙이면 다른 .cpp 파일에서 extern으로 참조할 수 없어
//   캡슐화 효과가 생긴다. (헤더의 private 멤버와 유사한 역할)

static Logger::Level g_log_level = Logger::Level::INFO;
// 기본값 INFO: DEBUG 로그는 verbose=true일 때만 보이도록

static std::string g_log_file = "";
// 빈 문자열이면 파일 출력 비활성화

// ── public API ────────────────────────────────────────────────────────────────
//
// 각 레벨 메서드는 단순히 log()로 위임한다.
// 이렇게 분리하는 이유: 호출 측 코드가 Logger::info("msg")처럼 간결하게
// 쓸 수 있고, 레벨 판단 로직이 log() 한 곳에만 있어 유지보수가 쉽다.

void Logger::debug(const std::string& msg)   { log(Level::DEBUG,   msg); }
void Logger::info(const std::string& msg)    { log(Level::INFO,    msg); }
void Logger::warning(const std::string& msg) { log(Level::WARNING, msg); }
void Logger::error(const std::string& msg)   { log(Level::ERROR,   msg); }

void Logger::set_level(Level level) {
    g_log_level = level;
}

void Logger::set_log_file(const std::string& path) {
    g_log_file = path;
}

// ── 핵심 출력 로직 ─────────────────────────────────────────────────────────────

void Logger::log(Level level, const std::string& msg) {
    // 현재 설정된 레벨보다 낮은 로그는 출력하지 않는다.
    // enum class는 정의 순서가 숫자 값과 일치하므로
    // DEBUG(0) < INFO(1) < WARNING(2) < ERROR(3) 비교가 가능하다.
    if (level < g_log_level) {
        return;
    }

    // 출력 포맷: "[LEVEL] YYYY-MM-DD HH:MM:SS message"
    // 예)        "[INFO] 2025-03-09 14:23:01 Proxy started"
    const std::string timestamp = get_timestamp();
    const std::string level_str = level_to_string(level);
    const std::string line = "[" + level_str + "] " + timestamp + " " + msg;

    // 콘솔 출력
    // endl 대신 "\n"을 쓰면 flush를 피할 수 있어 성능이 좋지만,
    // 로그는 즉시 보여야 하므로 flush가 보장되는 endl을 사용한다.
    std::cout << line << std::endl;

    // 파일 출력 (경로가 설정된 경우에만)
    // 매번 파일을 열고 닫는 이유:
    //   파일 핸들을 계속 열어두면 프로세스가 비정상 종료 시 마지막 로그가
    //   유실될 수 있다. 매번 열고 닫으면 비용이 있지만 데이터 안전성이 높다.
    //   Phase 11(안정성)에서 성능 개선 예정.
    if (!g_log_file.empty()) {
        // ios::app — 파일 끝에 이어쓰기. 기존 로그를 덮어쓰지 않는다.
        std::ofstream file(g_log_file, std::ios::app);
        if (file.is_open()) {
            file << line << std::endl;
        }
        // 파일 열기 실패는 무시한다. 로그 기록 실패로 프록시가 멈추면 안 되기 때문.
    }
}

// ── 헬퍼 함수 ─────────────────────────────────────────────────────────────────

std::string Logger::get_timestamp() {
    // std::time()    : Unix 타임스탬프(초 단위) 반환
    // std::localtime : 타임스탬프를 tm 구조체(년/월/일/시/분/초)로 변환
    // put_time       : tm 구조체를 포맷 문자열로 출력
    auto now = std::time(nullptr);
    auto tm  = *std::localtime(&now);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string Logger::level_to_string(Level level) {
    // switch로 처리하는 이유:
    //   if-else 체인보다 컴파일러가 점프 테이블로 최적화하기 쉽고,
    //   새 Level 추가 시 케이스 누락을 컴파일러 경고로 잡을 수 있다.
    switch (level) {
        case Level::DEBUG:   return "DEBUG";
        case Level::INFO:    return "INFO ";  // 공백으로 폭 맞춤
        case Level::WARNING: return "WARN ";
        case Level::ERROR:   return "ERROR";
        default:             return "?????";
    }
}
