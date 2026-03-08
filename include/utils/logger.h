#pragma once

#include <string>

/**
 * @file logger.h
 * @brief 전역 로깅 유틸리티
 *
 * 설계 결정:
 *   - 모든 메서드가 static인 이유:
 *     Logger 인스턴스를 어디서든 생성할 필요 없이 Logger::info("...") 형태로
 *     바로 호출하기 위함. 전역 상태(레벨, 파일 경로)를 파일 스코프 static 변수로
 *     관리하므로 인스턴스가 불필요하다.
 *
 *   - enum class Level을 쓰는 이유:
 *     일반 enum은 int로 암묵 변환돼 실수를 유발할 수 있다.
 *     enum class는 타입 안전성을 보장하고, Level::DEBUG처럼 명시적으로
 *     써야 하므로 코드의 의도가 명확해진다.
 *
 *   - 로그 레벨 순서 (DEBUG < INFO < WARNING < ERROR):
 *     현재 설정된 레벨보다 낮은 로그는 출력하지 않는다.
 *     예) set_level(WARNING) → DEBUG, INFO는 무시됨.
 *     개발 시에는 DEBUG, 운영 시에는 WARNING 이상만 출력하는 식으로 조절.
 */
class Logger {
public:
    /**
     * 로그 레벨 정의
     * 숫자가 낮을수록 상세 로그, 높을수록 중요 로그만 출력
     */
    enum class Level {
        DEBUG,    // 개발 중 상세 추적용 (함수 진입, 변수값 등)
        INFO,     // 정상 흐름 기록 (연결 수립, 설정 로드 등)
        WARNING,  // 잠재적 문제 (재시도, 예상 외 경로 등)
        ERROR     // 즉각 확인 필요한 오류 (소켓 실패, 파일 없음 등)
    };

    // ── 로그 출력 ─────────────────────────────────────────────────────────────

    /** 개발/디버깅용 상세 로그. 운영 환경에서는 보통 비활성화 */
    static void debug(const std::string& msg);

    /** 정상 동작을 기록하는 일반 로그 */
    static void info(const std::string& msg);

    /** 오류는 아니지만 주의가 필요한 상황 */
    static void warning(const std::string& msg);

    /** 즉시 확인이 필요한 오류. 연결 실패, 예외 등 */
    static void error(const std::string& msg);

    // ── 설정 ──────────────────────────────────────────────────────────────────

    /**
     * 출력할 최소 로그 레벨 설정
     * 이 레벨보다 낮은 로그는 무시된다.
     * 기본값: INFO (DEBUG 로그는 출력 안 됨)
     */
    static void set_level(Level level);

    /**
     * 로그를 파일에도 기록할 경로 설정
     * 빈 문자열("")이면 파일 출력 비활성화 (콘솔만 출력)
     * 파일은 append 모드로 열리므로 기존 내용이 유지된다.
     */
    static void set_log_file(const std::string& path);

private:
    /**
     * 실제 출력 로직 (콘솔 + 파일)
     * public 메서드(debug/info 등)가 모두 이 함수로 위임한다.
     */
    static void log(Level level, const std::string& msg);

    /**
     * 현재 시각을 "YYYY-MM-DD HH:MM:SS" 형식의 문자열로 반환
     * 로그 한 줄에 타임스탬프를 붙여 언제 발생한 이벤트인지 추적 가능하게 한다.
     */
    static std::string get_timestamp();

    /**
     * Level 열거값을 출력용 문자열로 변환
     * 예) Level::DEBUG → "DEBUG"
     */
    static std::string level_to_string(Level level);
};
