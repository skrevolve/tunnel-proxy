#include "utils/config.h"
#include <fstream>
#include <stdexcept>

// 임시: JSON 파싱 라이브러리 없이 간단하게 구현
// 나중에 nlohmann/json 같은 라이브러리 사용 권장

Config Config::load_from_file(const std::string& path) {
    // TODO: JSON 파일 파싱 구현
    
    // 힌트:
    // 1. std::ifstream으로 파일 열기
    // 2. JSON 파싱 (간단한 방법: 직접 파싱 또는 라이브러리 사용)
    // 3. Config 객체 생성 및 반환
    
    // 임시 구현: 기본값 반환
    Config config;
    config.local_port_ = 8080;
    config.target_ip_ = "127.0.0.1";
    config.target_port_ = 80;
    config.mode_ = "tcp";
    config.verbose_ = false;
    config.log_file_ = "proxy.log";
    
    // 파일 존재 확인
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }
    
    // TODO: 실제 파싱 구현
    // 지금은 기본값만 반환
    
    return config;
}
