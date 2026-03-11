#include <iostream>
#include <csignal>
#include "core/epoll_proxy.h"
#include "utils/config.h"
#include "utils/logger.h"

// ── 전역 프록시 포인터 ─────────────────────────────────────────────────────────
//
// 시그널 핸들러(signal_handler)는 일반 함수여야 하고,
// 클래스 멤버나 람다를 직접 등록할 수 없다.
// 핸들러 안에서 proxy.stop()을 호출하려면 proxy에 접근할 방법이 필요한데,
// 시그널 핸들러는 인자로 signum만 받을 수 있어 proxy를 전달할 수 없다.
// 그래서 전역 포인터를 통해 접근한다.
//
// 주의: 시그널 핸들러에서 호출 가능한 함수는 async-signal-safe 함수로 제한된다.
//       stop()은 atomic 쓰기 + shutdown() 호출이므로 사실상 안전하게 동작하나,
//       엄밀히는 async-signal-safe가 보장된 write() + _Exit() 정도만 권장된다.
//       Phase 11(안정성)에서 self-pipe trick으로 개선 예정.
EpollProxy* g_proxy = nullptr;

// ── 시그널 핸들러 ──────────────────────────────────────────────────────────────

void signal_handler(int signum) {
    // SIGINT (2) : Ctrl+C 입력 시 발생
    // SIGTERM(15): kill 명령이나 systemd stop 시 발생
    Logger::info("Received signal " + std::to_string(signum) + ", shutting down...");
    if (g_proxy) {
        g_proxy->stop();
        // stop()은 running_=false + shutdown(listen_fd_)를 수행해
        // run()의 accept() 루프를 빠져나오게 만든다.
    }
}

// ── 사용법 출력 ────────────────────────────────────────────────────────────────

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n"
              << "Options:\n"
              << "  -c, --config <file>   Config file path (default: config.json)\n"
              << "  -h, --help            Show this help\n"
              << "  -v, --verbose         Verbose logging\n";
}

// ── 진입점 ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // 명령줄 인자 파싱
    // -c / --config: 설정 파일 경로 지정 (기본값: config.json)
    // -h / --help  : 사용법 출력 후 종료
    std::string config_path = "config.json";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];  // 다음 인자를 경로로 사용
        }
    }

    // try-catch로 전체를 감싸는 이유:
    //   소켓 생성, 파일 파싱 등 초기화 단계에서 예외가 발생할 수 있다.
    //   예외를 잡아 Logger::error로 출력하고 종료 코드 1을 반환한다.
    //   예외를 잡지 않으면 std::terminate()가 호출되어 스택 트레이스가
    //   출력되고 종료되는데, 메시지가 불친절하다.
    try {
        // 1단계: 설정 로드
        Logger::info("Loading config from: " + config_path);
        auto config = Config::load_from_file(config_path);

        // verbose=true면 DEBUG 레벨로 낮춰 상세 로그 활성화
        if (config.is_verbose()) {
            Logger::set_level(Logger::Level::DEBUG);
        }

        // 로그 파일 경로 설정 (빈 문자열이면 콘솔만 출력)
        Logger::set_log_file(config.get_log_file());

        // 2단계: 프록시 생성 (생성자에서 소켓 바인딩까지 완료)
        // Phase 3-A 기준: EpollProxy (epoll ET + splice zero-copy 포워딩)
        // local_port, target_ip, target_port 모두 config.json에서 읽어온다.
        Logger::info("Starting proxy...");
        Logger::info("Local port: " + std::to_string(config.get_local_port()));
        Logger::info("Target: " + config.get_target_ip() + ":" +
                     std::to_string(config.get_target_port()));

        EpollProxy proxy(
            config.get_local_port(),
            config.get_target_ip(),
            config.get_target_port()
        );

        // 3단계: 시그널 핸들러 등록
        // proxy가 스택에 있으므로 g_proxy에 주소를 저장한다.
        // std::signal보다 sigaction이 더 안전하지만,
        // Phase 11(안정성)에서 sigaction + self-pipe trick으로 개선 예정.
        g_proxy = &proxy;
        std::signal(SIGINT,  signal_handler);
        std::signal(SIGTERM, signal_handler);

        // 4단계: 프록시 실행 (Ctrl+C 또는 stop()이 호출될 때까지 블로킹)
        proxy.run();

        // run()이 반환되면 정상 종료
        Logger::info("Proxy stopped");
        Logger::info("Total connections: " +
                     std::to_string(proxy.get_total_connections()));

    } catch (const std::exception& e) {
        Logger::error(std::string("Fatal error: ") + e.what());
        return 1;  // 비정상 종료 (쉘에서 $?로 확인 가능)
    }

    return 0;  // 정상 종료
}
