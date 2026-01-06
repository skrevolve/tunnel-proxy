#include "utils/logger.h"
#include <iostream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>

// 전역 설정
static Logger::Level g_log_level = Logger::Level::INFO;
static std::string g_log_file = "";

void Logger::debug(const std::string& msg) {
    log(Level::DEBUG, msg);
}

void Logger::info(const std::string& msg) {
    log(Level::INFO, msg);
}

void Logger::warning(const std::string& msg) {
    log(Level::WARNING, msg);
}

void Logger::error(const std::string& msg) {
    log(Level::ERROR, msg);
}

void Logger::set_level(Level level) {
    g_log_level = level;
}

void Logger::set_log_file(const std::string& path) {
    g_log_file = path;
}

void Logger::log(Level level, const std::string& msg) {
    // 로그 레벨 필터링
    if (level < g_log_level) {
        return;
    }
    
    // TODO: 로그 출력 구현
    
    // 힌트:
    // 1. get_timestamp() 호출
    // 2. level_to_string() 호출
    // 3. 콘솔 출력
    // 4. 파일 출력 (g_log_file이 비어있지 않으면)
    
    std::string timestamp = get_timestamp();
    std::string level_str = level_to_string(level);
    
    // 콘솔 출력
    std::cout << "[" << level_str << "] " << timestamp << " " << msg << std::endl;
    
    // 파일 출력
    if (!g_log_file.empty()) {
        std::ofstream file(g_log_file, std::ios::app);
        if (file.is_open()) {
            file << "[" << level_str << "] " << timestamp << " " << msg << std::endl;
        }
    }
}

std::string Logger::get_timestamp() {
    // TODO: 타임스탬프 생성
    
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string Logger::level_to_string(Level level) {
    switch (level) {
        case Level::DEBUG:   return "DEBUG";
        case Level::INFO:    return "INFO";
        case Level::WARNING: return "WARN";
        case Level::ERROR:   return "ERROR";
        default:             return "UNKNOWN";
    }
}
