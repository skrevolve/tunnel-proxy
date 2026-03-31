#include <iostream>
#include <csignal>
#include "core/tunnel_agent.h"
#include "utils/config.h"
#include "utils/logger.h"

static proxy::TunnelAgent* g_agent = nullptr;

void signal_handler(int signum) {
    Logger::info("Received signal " + std::to_string(signum) + ", shutting down...");
    if (g_agent) g_agent->stop();
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n"
              << "Options:\n"
              << "  -c, --config <file>   Config file path (default: agent.json)\n"
              << "  -h, --help            Show this help\n";
}

int main(int argc, char* argv[]) {
    std::string config_path = "agent.json";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    try {
        Logger::info("Loading config from: " + config_path);
        auto config = Config::load_from_file(config_path);

        if (config.is_verbose()) Logger::set_level(Logger::Level::DEBUG);
        Logger::set_log_file(config.get_log_file());

        Logger::info("Tunnel agent: server=" + config.get_server_ip() + ":"
                     + std::to_string(config.get_server_port())
                     + ", id=" + config.get_agent_id());

        proxy::TunnelAgent agent(config.get_server_ip(),
                                 config.get_server_port(),
                                 config.get_agent_id());
        g_agent = &agent;

        std::signal(SIGINT,  signal_handler);
        std::signal(SIGTERM, signal_handler);

        agent.run();  // 지수 백오프 자동 재연결, stop() 호출 시에만 종료

    } catch (const std::exception& e) {
        Logger::error(std::string("Fatal error: ") + e.what());
        return 1;
    }

    return 0;
}
