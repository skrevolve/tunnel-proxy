#include <gtest/gtest.h>
#include "utils/config.h"
#include <fstream>
#include <cstdio>

static std::string write_temp(const std::string& content) {
    std::string path = "/tmp/test_config_" + std::to_string(::getpid()) + ".json";
    std::ofstream f(path);
    f << content;
    return path;
}

// ─── proxy 설정 파싱 ──────────────────────────────────────────────────────────

TEST(ConfigTest, ParsesProxyFields) {
    auto path = write_temp(R"({
        "agent_port": 9900,
        "proxy_port": 9901,
        "guac_port":  8765
    })");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.get_agent_port(), 9900);
    EXPECT_EQ(cfg.get_proxy_port(), 9901);
    EXPECT_EQ(cfg.get_guac_port(),  8765);
    std::remove(path.c_str());
}

TEST(ConfigTest, ProxyFieldsUseDefaults) {
    auto path = write_temp(R"({})");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.get_agent_port(), 9900);
    EXPECT_EQ(cfg.get_proxy_port(), 9901);
    EXPECT_EQ(cfg.get_guac_port(),  8765);
    std::remove(path.c_str());
}

// ─── agent 설정 파싱 ──────────────────────────────────────────────────────────

TEST(ConfigTest, ParsesAgentFields) {
    auto path = write_temp(R"({
        "server_ip":   "10.0.0.1",
        "server_port": 9900,
        "agent_id":    "my-agent"
    })");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.get_server_ip(),   "10.0.0.1");
    EXPECT_EQ(cfg.get_server_port(), 9900);
    EXPECT_EQ(cfg.get_agent_id(),    "my-agent");
    std::remove(path.c_str());
}

TEST(ConfigTest, AgentFieldsUseDefaults) {
    auto path = write_temp(R"({})");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.get_server_ip(),   "127.0.0.1");
    EXPECT_EQ(cfg.get_server_port(), 9900);
    EXPECT_EQ(cfg.get_agent_id(),    "agent-1");
    std::remove(path.c_str());
}

// ─── 공통 선택 필드 ───────────────────────────────────────────────────────────

TEST(ConfigTest, ParsesCommonOptionalFields) {
    auto path = write_temp(R"({
        "verbose":  true,
        "log_file": "proxy.log"
    })");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.is_verbose(),   true);
    EXPECT_EQ(cfg.get_log_file(), "proxy.log");
    std::remove(path.c_str());
}

TEST(ConfigTest, CommonOptionalFieldsUseDefaults) {
    auto path = write_temp(R"({})");
    auto cfg = Config::load_from_file(path);
    EXPECT_EQ(cfg.is_verbose(),   false);
    EXPECT_EQ(cfg.get_log_file(), "");
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
