#include "core/access_policy.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace proxy {

// ── 규칙 관리 ─────────────────────────────────────────────────────────────────

void AccessPolicy::add_rule(Rule rule) {
    rules_.push_back(std::move(rule));
}

// ── JSON 파싱 ─────────────────────────────────────────────────────────────────

AccessPolicy AccessPolicy::parse_json(const std::string& json_str) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(std::string("AccessPolicy: JSON parse error: ") + e.what());
    }

    if (!j.contains("rules") || !j["rules"].is_array()) {
        throw std::runtime_error("AccessPolicy: missing or invalid 'rules' array");
    }

    AccessPolicy policy;
    for (const auto& r : j["rules"]) {
        Rule rule;

        if (!r.contains("subject") || !r["subject"].is_string())
            throw std::runtime_error("AccessPolicy: rule missing 'subject'");
        if (!r.contains("tunnel_id") || !r["tunnel_id"].is_string())
            throw std::runtime_error("AccessPolicy: rule missing 'tunnel_id'");
        if (!r.contains("target_ip") || !r["target_ip"].is_string())
            throw std::runtime_error("AccessPolicy: rule missing 'target_ip'");
        if (!r.contains("target_port") || !r["target_port"].is_number_unsigned())
            throw std::runtime_error("AccessPolicy: rule missing 'target_port'");

        rule.subject     = r["subject"].get<std::string>();
        rule.tunnel_id   = r["tunnel_id"].get<std::string>();
        rule.target_ip   = r["target_ip"].get<std::string>();
        rule.target_port = r["target_port"].get<uint16_t>();

        policy.rules_.push_back(std::move(rule));
    }
    return policy;
}

AccessPolicy AccessPolicy::load_from_string(const std::string& json_str) {
    return parse_json(json_str);
}

AccessPolicy AccessPolicy::load_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("AccessPolicy: cannot open file: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_json(ss.str());
}

// ── 접근 제어 판단 ────────────────────────────────────────────────────────────

bool AccessPolicy::matches(const Rule&        rule,
                           const std::string& subject,
                           const std::string& tunnel_id,
                           const std::string& target_ip,
                           uint16_t           target_port) {
    if (rule.subject   != "*" && rule.subject   != subject)   return false;
    if (rule.tunnel_id != "*" && rule.tunnel_id != tunnel_id) return false;
    if (rule.target_ip != "*" && rule.target_ip != target_ip) return false;
    if (rule.target_port != 0 && rule.target_port != target_port) return false;
    return true;
}

bool AccessPolicy::is_allowed(const std::string& subject,
                               const std::string& tunnel_id,
                               const std::string& target_ip,
                               uint16_t           target_port) const {
    for (const auto& rule : rules_) {
        if (matches(rule, subject, tunnel_id, target_ip, target_port)) {
            return true;
        }
    }
    return false;  // default-deny
}

} // namespace proxy
