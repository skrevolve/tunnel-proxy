#include <iostream>
#include <csignal>
#include "core/epoll_proxy.h"
#include "core/tls_proxy.h"
#include "core/tunnel_server.h"
#include "core/guac_websocket.h"
#include "utils/config.h"
#include "utils/logger.h"

// ── 전역 포인터 (시그널 핸들러용) ──────────────────────────────────────────────
static EpollProxy*                    g_epoll_proxy = nullptr;
static TlsProxy*                      g_tls_proxy   = nullptr;
static proxy::TunnelServer*           g_tunnel_srv  = nullptr;
static proxy::GuacWebSocketGateway*   g_gateway     = nullptr;

void signal_handler(int signum) {
    Logger::info("Received signal " + std::to_string(signum) + ", shutting down...");
    if (g_gateway)     g_gateway->stop();
    if (g_epoll_proxy) g_epoll_proxy->stop();
    if (g_tls_proxy)   g_tls_proxy->stop();
    if (g_tunnel_srv)  g_tunnel_srv->stop();
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n"
              << "Options:\n"
              << "  -c, --config <file>   Config file path (default: config.json)\n"
              << "  -h, --help            Show this help\n"
              << "\n"
              << "Modes (config.json \"mode\" field):\n"
              << "  tcp            — EpollProxy:   TCP 포워딩 (기본)\n"
              << "  tls            — TlsProxy:     TLS 암호화 TCP 포워딩\n"
              << "  tunnel-server  — TunnelServer: 리버스 터널 서버\n"
              << "\n"
              << "Guacamole WebSocket gateway always starts on port 8765.\n"
              << "For tunnel agent, use the 'agent' binary.\n";
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

        const std::string mode = config.get_mode();
        Logger::info("Mode: " + mode);

        // ── Guacamole WebSocket 게이트웨이 — 모든 모드에서 공통 시작 ───────────
        proxy::GuacWebSocketGateway gateway;
        gateway.start(8765);
        g_gateway = &gateway;
        Logger::info("Guacamole WebSocket gateway listening on port 8765");

        std::signal(SIGINT,  signal_handler);
        std::signal(SIGTERM, signal_handler);

        // ── 모드별 컴포넌트 시작 ─────────────────────────────────────────────
        if (mode == "tcp") {
            Logger::info("TCP proxy: " + config.get_target_ip() + ":"
                         + std::to_string(config.get_target_port())
                         + " ← port " + std::to_string(config.get_local_port()));

            EpollProxy proxy(config.get_local_port(),
                             config.get_target_ip(),
                             config.get_target_port());
            g_epoll_proxy = &proxy;
            proxy.run();
            Logger::info("Proxy stopped. Total connections: "
                         + std::to_string(proxy.get_total_connections()));

        } else if (mode == "tls") {
            Logger::info("TLS proxy: " + config.get_target_ip() + ":"
                         + std::to_string(config.get_target_port())
                         + " ← port " + std::to_string(config.get_local_port()));
            Logger::info("Cert: " + config.get_cert_file()
                         + ", Key: " + config.get_key_file());

            TlsProxy proxy(config.get_local_port(),
                           config.get_target_ip(),
                           config.get_target_port(),
                           config.get_cert_file(),
                           config.get_key_file());
            g_tls_proxy = &proxy;
            proxy.run();

        } else if (mode == "tunnel-server") {
            Logger::info("Tunnel server: agent_port=" + std::to_string(config.get_agent_port())
                         + ", proxy_port=" + std::to_string(config.get_proxy_port()));

            proxy::TunnelServer server(config.get_agent_port(),
                                       config.get_proxy_port());
            g_tunnel_srv = &server;
            server.run();

        } else {
            Logger::error("Unknown mode: " + mode
                          + ". Valid modes: tcp, tls, tunnel-server");
            return 1;
        }

    } catch (const std::exception& e) {
        Logger::error(std::string("Fatal error: ") + e.what());
        return 1;
    }

    return 0;
}
