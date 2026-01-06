#pragma once

#include <string>

/**
 * 로깅 유틸리티
 * 
 * 신입 작업:
 * - 콘솔/파일 로깅
 * - 타임스탬프 추가
 * - 로그 레벨 관리
 */
class Logger {
public:
    enum class Level {
        DEBUG,
        INFO,
        WARNING,
        ERROR
    };
    
    // 로그 출력 메서드
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warning(const std::string& msg);
    static void error(const std::string& msg);
    
    // 설정
    static void set_level(Level level);
    static void set_log_file(const std::string& path);

private:
    // TODO: 구현
    static void log(Level level, const std::string& msg);
    static std::string get_timestamp();
    static std::string level_to_string(Level level);
};
