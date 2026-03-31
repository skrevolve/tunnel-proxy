#include "utils/config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

Config Config::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error("JSON parse error in " + path + ": " + e.what());
    }

    Config config;

    // proxy 설정
    config.agent_port_ = j.value("agent_port", 9900);
    config.proxy_port_ = j.value("proxy_port", 9901);
    config.guac_port_  = j.value("guac_port",  8765);

    // agent 설정
    config.server_ip_   = j.value("server_ip",   std::string("127.0.0.1"));
    config.server_port_ = j.value("server_port", 9900);
    config.agent_id_    = j.value("agent_id",    std::string("agent-1"));

    // 공통
    config.verbose_      = j.value("verbose",      false);
    config.log_file_     = j.value("log_file",     std::string(""));
    config.web_renderer_ = j.value("web_renderer", std::string("http"));

    return config;
}
