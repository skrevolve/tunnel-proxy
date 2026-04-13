#include <iostream>
#include <csignal>
#include "core/tunnel_server.h"
#include "core/guac_websocket.h"
#include "utils/config.h"
#include "utils/logger.h"

static proxy::TunnelServer*         g_tunnel_srv = nullptr;
static proxy::GuacWebSocketGateway* g_gateway    = nullptr;

void signal_handler(int signum) {
    Logger::info("Received signal " + std::to_string(signum) + ", shutting down...");
    if (g_gateway)    g_gateway->stop();
    if (g_tunnel_srv) g_tunnel_srv->stop();
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n"
              << "Options:\n"
              << "  -c, --config <file>   Config file path (default: config.json)\n"
              << "  -h, --help            Show this help\n"
              << "\n"
              << "Ports:\n"
              << "  agent_port (default 9900) — agent 역방향 연결 수신\n"
              << "  proxy_port (default 9901) — 외부 클라이언트 터널 진입\n"
              << "  guac_port  (default 8765) — 브라우저 WebSocket 연결\n";
}

int main(int argc, char* argv[]) {
    std::string config_path = "config.json";

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

        // ── TunnelServer ─────────────────────────────────────────────────────
        // GuacWebSocketGateway보다 먼저 생성해야 한다.
        // gateway.set_tunnel_server()에 포인터를 넘기므로
        // tunnel_server가 gateway보다 오래 살아야 한다 (같은 스코프에서 스택 역순 소멸).
        proxy::TunnelServer tunnel_server(config.get_agent_port(),
                                          config.get_proxy_port());
        g_tunnel_srv = &tunnel_server;

        // ── GuacWebSocketGateway ─────────────────────────────────────────────
        proxy::GuacWebSocketGateway gateway;
        gateway.set_web_renderer(config.get_web_renderer());
        gateway.set_tunnel_server(&tunnel_server);
        gateway.start(config.get_guac_port());
        g_gateway = &gateway;
        Logger::info("Guacamole WebSocket gateway listening on port "
                     + std::to_string(config.get_guac_port())
                     + " (web_renderer=" + config.get_web_renderer() + ")");
        Logger::info("TunnelServer: agent_port=" + std::to_string(config.get_agent_port())
                     + ", proxy_port=" + std::to_string(config.get_proxy_port()));

        std::signal(SIGINT,  signal_handler);
        std::signal(SIGTERM, signal_handler);

        tunnel_server.run();  // 블로킹 — SIGINT/SIGTERM까지 실행

    } catch (const std::exception& e) {
        Logger::error(std::string("Fatal error: ") + e.what());
        return 1;
    }

    return 0;
}
