#pragma once

#include <string>

/**
 * 설정 파일 파서
 * 
 * 신입 작업:
 * - JSON 파일에서 설정 읽기
 * - 설정 값 검증
 */
class Config {
public:
    // 설정 파일 로드
    static Config load_from_file(const std::string& path);
    
    // Getter 메서드
    int get_local_port() const { return local_port_; }
    std::string get_target_ip() const { return target_ip_; }
    int get_target_port() const { return target_port_; }
    std::string get_mode() const { return mode_; }
    bool is_verbose() const { return verbose_; }
    std::string get_log_file() const { return log_file_; }

private:
    // TODO: 설정 값들
    int local_port_;
    std::string target_ip_;
    int target_port_;
    std::string mode_;  // "tcp" or "udp"
    bool verbose_;
    std::string log_file_;
};
