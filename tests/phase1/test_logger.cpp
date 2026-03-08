#include <gtest/gtest.h>
#include "utils/logger.h"
#include <fstream>
#include <string>
#include <cstdio>

// ─── 로그 레벨 필터링 ────────────────────────────────────────────────────────

TEST(LoggerTest, LevelFilterIgnoresLower) {
    // WARNING 레벨 설정 시 DEBUG/INFO는 무시 — 단순히 크래시 없이 통과하면 OK
    Logger::set_level(Logger::Level::WARNING);
    EXPECT_NO_THROW(Logger::debug("should be filtered"));
    EXPECT_NO_THROW(Logger::info("should be filtered"));
    EXPECT_NO_THROW(Logger::warning("should pass"));
    EXPECT_NO_THROW(Logger::error("should pass"));
    Logger::set_level(Logger::Level::INFO);  // 복원
}

TEST(LoggerTest, AllLevelsPassWhenSetToDebug) {
    Logger::set_level(Logger::Level::DEBUG);
    EXPECT_NO_THROW(Logger::debug("debug msg"));
    EXPECT_NO_THROW(Logger::info("info msg"));
    EXPECT_NO_THROW(Logger::warning("warning msg"));
    EXPECT_NO_THROW(Logger::error("error msg"));
    Logger::set_level(Logger::Level::INFO);  // 복원
}

// ─── 파일 출력 ───────────────────────────────────────────────────────────────

TEST(LoggerTest, WritesToFile) {
    const std::string path = "/tmp/test_logger_" + std::to_string(::getpid()) + ".log";
    std::remove(path.c_str());

    Logger::set_log_file(path);
    Logger::set_level(Logger::Level::DEBUG);
    Logger::info("hello from test");
    Logger::set_log_file("");  // 복원

    std::ifstream f(path);
    ASSERT_TRUE(f.is_open()) << "로그 파일이 생성되지 않음";

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("hello from test"), std::string::npos);

    std::remove(path.c_str());
}

TEST(LoggerTest, FileContainsLevelAndTimestamp) {
    const std::string path = "/tmp/test_logger_ts_" + std::to_string(::getpid()) + ".log";
    std::remove(path.c_str());

    Logger::set_log_file(path);
    Logger::set_level(Logger::Level::DEBUG);
    Logger::error("critical failure");
    Logger::set_log_file("");

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("ERROR"), std::string::npos);
    EXPECT_NE(content.find("critical failure"), std::string::npos);

    std::remove(path.c_str());
}
