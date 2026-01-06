#include <iostream>
#include <csignal>
#include "core/basic_proxy.h"
#include "utils/config.h"
#include "utils/logger.h"

// 전역 프록시 포인터 (시그널 핸들러용)
BasicProxy* g_proxy = nullptr;

void signal_handler(int signum) {
    Logger::info("Received signal " + std::to_string(signum) + ", shutting down...");
    if (g_proxy) {
        g_proxy->stop();
    }
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n"
              << "Options:\n"
              << "  -c, --config <file>   Config file path (default: config.json)\n"
              << "  -h, --help            Show this help\n"
              << "  -v, --verbose         Verbose logging\n";
}

int main(int argc, char* argv[]) {
    // TODO: 명령줄 인자 파싱
    std::string config_path = "config.json";
    
    // 간단한 인자 처리
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_path = argv[++i];
            }
        }
    }
    
    try {
        // 설정 로드
        Logger::info("Loading config from: " + config_path);
        auto config = Config::load_from_file(config_path);
        
        if (config.is_verbose()) {
            Logger::set_level(Logger::Level::DEBUG);
        }
        
        Logger::set_log_file(config.get_log_file());
        
        // 프록시 생성
        Logger::info("Starting proxy...");
        Logger::info("Local port: " + std::to_string(config.get_local_port()));
        Logger::info("Target: " + config.get_target_ip() + ":" + 
                     std::to_string(config.get_target_port()));
        
        BasicProxy proxy(
            config.get_local_port(),
            config.get_target_ip(),
            config.get_target_port()
        );
        
        g_proxy = &proxy;
        
        // 시그널 핸들러 등록
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        
        // 프록시 시작
        proxy.run();
        
        Logger::info("Proxy stopped");
        Logger::info("Total connections: " + 
                     std::to_string(proxy.get_total_connections()));
        
    } catch (const std::exception& e) {
        Logger::error(std::string("Fatal error: ") + e.what());
        return 1;
    }
    
    return 0;
}
