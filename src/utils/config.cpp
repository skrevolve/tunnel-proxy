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
    config.local_port_  = j.at("local_port").get<int>();
    config.target_ip_   = j.at("target_ip").get<std::string>();
    config.target_port_ = j.at("target_port").get<int>();
    config.mode_        = j.value("mode", "tcp");
    config.verbose_     = j.value("verbose", false);
    config.log_file_    = j.value("log_file", "");

    return config;
}
