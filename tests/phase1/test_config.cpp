#include <gtest/gtest.h>
#include "utils/config.h"
#include <fstream>
#include <cstdio>

// 임시 파일 헬퍼
static std::string write_temp(const std::string& content) {
    std::string path = "/tmp/test_config_" + std::to_string(::getpid()) + ".json";
    std::ofstream f(path);
    f << content;
    return path;
}

// ─── 정상 파싱 ───────────────────────────────────────────────────────────────

TEST(ConfigTest, ParsesRequiredFields) {
    auto path = write_temp(R"({
        "local_port": 9090,
        "target_ip": "192.168.1.1",
        "target_port": 9091
    })");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.get_local_port(),  9090);
    EXPECT_EQ(cfg.get_target_ip(),   "192.168.1.1");
    EXPECT_EQ(cfg.get_target_port(), 9091);
    std::remove(path.c_str());
}

TEST(ConfigTest, OptionalFieldsUseDefaults) {
    auto path = write_temp(R"({
        "local_port": 8080,
        "target_ip": "127.0.0.1",
        "target_port": 80
    })");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.get_mode(),     "tcp");
    EXPECT_EQ(cfg.is_verbose(),   false);
    EXPECT_EQ(cfg.get_log_file(), "");
    std::remove(path.c_str());
}

TEST(ConfigTest, ParsesOptionalFields) {
    auto path = write_temp(R"({
        "local_port": 8080,
        "target_ip": "127.0.0.1",
        "target_port": 80,
        "mode": "udp",
        "verbose": true,
        "log_file": "proxy.log"
    })");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.get_mode(),     "udp");
    EXPECT_EQ(cfg.is_verbose(),   true);
    EXPECT_EQ(cfg.get_log_file(), "proxy.log");
    std::remove(path.c_str());
}

// ─── 에러 처리 ───────────────────────────────────────────────────────────────

TEST(ConfigTest, ThrowsOnMissingFile) {
    EXPECT_THROW(
        Config::load_from_file("/nonexistent/path/config.json"),
        std::runtime_error
    );
}

TEST(ConfigTest, ThrowsOnInvalidJson) {
    auto path = write_temp("{ invalid json }");
    EXPECT_THROW(Config::load_from_file(path), std::runtime_error);
    std::remove(path.c_str());
}

TEST(ConfigTest, ThrowsOnMissingRequiredField) {
    // local_port 누락
    auto path = write_temp(R"({
        "target_ip": "127.0.0.1",
        "target_port": 80
    })");
    EXPECT_THROW(Config::load_from_file(path), std::exception);
    std::remove(path.c_str());
}
