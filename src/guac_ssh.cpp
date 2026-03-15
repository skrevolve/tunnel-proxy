#include "core/guac_ssh.h"

#include <libssh2.h>

#include <openssl/evp.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>

#include <stdexcept>
#include <cstring>
#include <vector>

namespace proxy {

// ── pImpl ────────────────────────────────────────────────────────────────────

struct GuacSshClient::Impl {
    int              sock{-1};
    LIBSSH2_SESSION* session{nullptr};
    std::atomic<bool> stop{false};
};

// ── 생성자 / 소멸자 ──────────────────────────────────────────────────────────

GuacSshClient::GuacSshClient(InstructionCallback callback)
    : callback_(std::move(callback))
    , impl_(std::make_unique<Impl>())
{}

GuacSshClient::~GuacSshClient() {
    disconnect();
}

// ── public API ───────────────────────────────────────────────────────────────

void GuacSshClient::connect(const std::string& host, uint16_t port,
                             const std::string& username,
                             const std::string& password,
                             uint16_t cols, uint16_t rows)
{
    worker_ = std::thread(&GuacSshClient::run_event_loop, this,
                          host, port, username, password, cols, rows);
}

void GuacSshClient::connect_with_key(const std::string& host, uint16_t port,
                                      const std::string& username,
                                      const std::string& privkey_path,
                                      uint16_t cols, uint16_t rows)
{
    worker_ = std::thread(&GuacSshClient::run_event_loop_key, this,
                          host, port, username, privkey_path, cols, rows);
}

void GuacSshClient::disconnect() {
    if (impl_) impl_->stop = true;
    if (worker_.joinable()) worker_.join();
}

bool GuacSshClient::is_connected() const {
    return connected_.load();
}

// send_input: 입력을 큐에만 push하고 즉시 반환.
// 실제 libssh2_channel_write는 worker_ 스레드에서 수행한다.
// (libssh2는 thread-safe하지 않으므로 단일 스레드 접근 필수)
void GuacSshClient::send_input(const std::string& data) {
    if (!connected_.load() || data.empty()) return;
    std::lock_guard<std::mutex> lock(input_queue_mutex_);
    input_queue_.push(data);
}

// ── base64 인코딩 (OpenSSL EVP) ───────────────────────────────────────────────

static std::string base64_encode(const unsigned char* data, size_t len) {
    if (len == 0) return {};
    std::vector<unsigned char> buf(((len + 2) / 3) * 4 + 1);
    int out_len = EVP_EncodeBlock(buf.data(), data, static_cast<int>(len));
    return std::string(reinterpret_cast<char*>(buf.data()),
                       static_cast<size_t>(out_len));
}

// ── flush_terminal_output ─────────────────────────────────────────────────────

void GuacSshClient::flush_terminal_output(const char* data, size_t len) {
    constexpr size_t CHUNK = 8192;
    for (size_t offset = 0; offset < len; offset += CHUNK) {
        size_t chunk_len = std::min(CHUNK, len - offset);
        std::string b64 = base64_encode(
            reinterpret_cast<const unsigned char*>(data + offset), chunk_len);
        GuacInstruction blob;
        blob.opcode = "blob";
        blob.args   = { std::to_string(stream_id_), std::move(b64) };
        callback_(blob);
    }
}

// ── TCP 소켓 생성 ──────────────────────────────────────────────────────────────

static int tcp_connect(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
        throw std::runtime_error("tcp_connect: getaddrinfo failed for " + host);

    int sock = -1;
    for (addrinfo* rp = res; rp; rp = rp->ai_next) {
        sock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (::connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        ::close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0)
        throw std::runtime_error("tcp_connect: cannot connect to " + host + ":" + port_str);
    return sock;
}

// ── select 기반 I/O 이벤트 루프 ───────────────────────────────────────────────
//
// libssh2를 단일 스레드(worker_)에서만 사용한다.
// - SSH 출력: select로 소켓 가독성 감지 → libssh2_channel_read (EAGAIN까지 드레인)
// - SSH 입력: select 후 input_queue_ 드레인 → libssh2_channel_write
//
// 이 구조로 libssh2_channel_read/write 간 데이터 레이스가 완전히 제거된다.

static void run_io_loop(int sock,
                        LIBSSH2_SESSION* session,
                        LIBSSH2_CHANNEL* ch,
                        std::atomic<bool>& stop,
                        std::mutex& queue_mutex,
                        std::queue<std::string>& queue,
                        std::atomic<bool>& connected,
                        std::function<void(const char*, size_t)> on_output)
{
    (void)session; // 향후 확장용 (keepalive 등)

    char buf[4096];
    bool eof = false;

    while (!stop.load() && !eof) {
        // 큐에 대기 중인 입력이 있는지 확인
        bool has_input = false;
        {
            std::lock_guard<std::mutex> lk(queue_mutex);
            has_input = !queue.empty();
        }

        // select: 출력 대기 (항상) + 입력 가능 여부 (큐가 있을 때)
        fd_set rd, wr;
        FD_ZERO(&rd); FD_ZERO(&wr);
        FD_SET(sock, &rd);
        if (has_input) FD_SET(sock, &wr);
        timeval tv = {0, 20000}; // 20ms — stop 플래그 폴링 간격
        select(sock + 1, &rd, has_input ? &wr : nullptr, nullptr, &tv);

        // SSH 출력 드레인: EAGAIN이 올 때까지 읽기
        while (true) {
            ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf));
            if (n == LIBSSH2_ERROR_EAGAIN) break;
            if (n <= 0) { eof = true; break; }
            on_output(buf, static_cast<size_t>(n));
        }

        // 입력 큐 드레인: 큐가 비거나 EAGAIN이 올 때까지 쓰기
        while (!eof) {
            std::string data;
            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                if (queue.empty()) break;
                data = queue.front(); // pop 전에 복사 (EAGAIN 시 재시도 대비)
            }
            ssize_t n = libssh2_channel_write(ch, data.data(), data.size());
            if (n == LIBSSH2_ERROR_EAGAIN) break; // 다음 루프에서 재시도
            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                if (!queue.empty()) queue.pop();
            }
            if (n < 0) { eof = true; break; }
        }

        if (libssh2_channel_eof(ch)) break;
    }

    connected.store(false);
}

// ── run_event_loop (비밀번호 인증) ─────────────────────────────────────────────

void GuacSshClient::run_event_loop(const std::string& host, uint16_t port,
                                   const std::string& username,
                                   const std::string& password,
                                   uint16_t cols, uint16_t rows)
{
    // 1. libssh2 초기화
    if (libssh2_init(0) != 0) return;

    // 2. TCP 연결
    try { impl_->sock = tcp_connect(host, port); }
    catch (...) { libssh2_exit(); return; }

    // 3. SSH 세션 + 논블로킹 핸드셰이크
    impl_->session = libssh2_session_init();
    if (!impl_->session) {
        ::close(impl_->sock); impl_->sock = -1;
        libssh2_exit(); return;
    }
    libssh2_session_set_blocking(impl_->session, 0);

    { int rc; while ((rc = libssh2_session_handshake(impl_->session, impl_->sock))
                     == LIBSSH2_ERROR_EAGAIN) {}
      if (rc != 0) {
          libssh2_session_free(impl_->session); impl_->session = nullptr;
          ::close(impl_->sock); impl_->sock = -1;
          libssh2_exit(); return;
      }
    }

    // 4. 비밀번호 인증
    { int rc; while ((rc = libssh2_userauth_password(impl_->session,
                                                     username.c_str(),
                                                     password.c_str()))
                     == LIBSSH2_ERROR_EAGAIN) {}
      if (rc != 0) {
          libssh2_session_disconnect(impl_->session, "auth failed");
          libssh2_session_free(impl_->session); impl_->session = nullptr;
          ::close(impl_->sock); impl_->sock = -1;
          libssh2_exit(); return;
      }
    }

    // 5. 채널 오픈
    LIBSSH2_CHANNEL* ch = nullptr;
    while (!ch) {
        ch = libssh2_channel_open_session(impl_->session);
        if (!ch) {
            int err = libssh2_session_last_error(impl_->session, nullptr, nullptr, 0);
            if (err == LIBSSH2_ERROR_EAGAIN) continue;
            libssh2_session_disconnect(impl_->session, "channel open failed");
            libssh2_session_free(impl_->session); impl_->session = nullptr;
            ::close(impl_->sock); impl_->sock = -1;
            libssh2_exit(); return;
        }
    }

    // 6. PTY 요청
    { int rc; while ((rc = libssh2_channel_request_pty_ex(ch, "xterm", 5,
                                                           nullptr, 0,
                                                           cols, rows, 0, 0))
                     == LIBSSH2_ERROR_EAGAIN) {}
      if (rc != 0) {
          libssh2_channel_free(ch);
          libssh2_session_disconnect(impl_->session, "pty failed");
          libssh2_session_free(impl_->session); impl_->session = nullptr;
          ::close(impl_->sock); impl_->sock = -1;
          libssh2_exit(); return;
      }
    }

    // 7. 셸 시작
    { int rc; while ((rc = libssh2_channel_shell(ch)) == LIBSSH2_ERROR_EAGAIN) {}
      if (rc != 0) {
          libssh2_channel_free(ch);
          libssh2_session_disconnect(impl_->session, "shell failed");
          libssh2_session_free(impl_->session); impl_->session = nullptr;
          ::close(impl_->sock); impl_->sock = -1;
          libssh2_exit(); return;
      }
    }

    connected_.store(true);

    // 8. Guacamole 연결 수립 명령어
    { GuacInstruction s; s.opcode="size";
      s.args={std::to_string(stream_id_),std::to_string(cols),std::to_string(rows)};
      callback_(s); }
    { GuacInstruction p; p.opcode="pipe";
      p.args={std::to_string(stream_id_),"terminal","text/plain"};
      callback_(p); }

    // 9. select 기반 I/O 루프 (읽기/쓰기 모두 worker_ 스레드에서)
    run_io_loop(impl_->sock, impl_->session, ch,
                impl_->stop, input_queue_mutex_, input_queue_, connected_,
                [this](const char* d, size_t l){ flush_terminal_output(d, l); });

    // 10. 종료
    { GuacInstruction e; e.opcode="end";
      e.args={std::to_string(stream_id_)}; callback_(e); }

    libssh2_channel_send_eof(ch);
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);

    libssh2_session_disconnect(impl_->session, "Normal Shutdown");
    libssh2_session_free(impl_->session); impl_->session = nullptr;
    ::close(impl_->sock); impl_->sock = -1;
    libssh2_exit();
}

// ── run_event_loop_key (공개키 인증) ──────────────────────────────────────────

void GuacSshClient::run_event_loop_key(const std::string& host, uint16_t port,
                                        const std::string& username,
                                        const std::string& privkey_path,
                                        uint16_t cols, uint16_t rows)
{
    if (libssh2_init(0) != 0) return;

    try { impl_->sock = tcp_connect(host, port); }
    catch (...) { libssh2_exit(); return; }

    impl_->session = libssh2_session_init();
    if (!impl_->session) {
        ::close(impl_->sock); impl_->sock = -1;
        libssh2_exit(); return;
    }
    libssh2_session_set_blocking(impl_->session, 0);

    { int rc; while ((rc = libssh2_session_handshake(impl_->session, impl_->sock))
                     == LIBSSH2_ERROR_EAGAIN) {}
      if (rc != 0) {
          libssh2_session_free(impl_->session); impl_->session = nullptr;
          ::close(impl_->sock); impl_->sock = -1;
          libssh2_exit(); return;
      }
    }

    std::string pubkey_path = privkey_path + ".pub";
    { int rc; while ((rc = libssh2_userauth_publickey_fromfile(impl_->session,
                                                                username.c_str(),
                                                                pubkey_path.c_str(),
                                                                privkey_path.c_str(),
                                                                nullptr))
                     == LIBSSH2_ERROR_EAGAIN) {}
      if (rc != 0) {
          libssh2_session_disconnect(impl_->session, "auth failed");
          libssh2_session_free(impl_->session); impl_->session = nullptr;
          ::close(impl_->sock); impl_->sock = -1;
          libssh2_exit(); return;
      }
    }

    LIBSSH2_CHANNEL* ch = nullptr;
    while (!ch) {
        ch = libssh2_channel_open_session(impl_->session);
        if (!ch) {
            int err = libssh2_session_last_error(impl_->session, nullptr, nullptr, 0);
            if (err == LIBSSH2_ERROR_EAGAIN) continue;
            libssh2_session_disconnect(impl_->session, "channel open failed");
            libssh2_session_free(impl_->session); impl_->session = nullptr;
            ::close(impl_->sock); impl_->sock = -1;
            libssh2_exit(); return;
        }
    }

    { int rc; while ((rc = libssh2_channel_request_pty_ex(ch, "xterm", 5,
                                                           nullptr, 0,
                                                           cols, rows, 0, 0))
                     == LIBSSH2_ERROR_EAGAIN) {}
      if (rc != 0) {
          libssh2_channel_free(ch);
          libssh2_session_disconnect(impl_->session, "pty failed");
          libssh2_session_free(impl_->session); impl_->session = nullptr;
          ::close(impl_->sock); impl_->sock = -1;
          libssh2_exit(); return;
      }
    }

    { int rc; while ((rc = libssh2_channel_shell(ch)) == LIBSSH2_ERROR_EAGAIN) {}
      if (rc != 0) {
          libssh2_channel_free(ch);
          libssh2_session_disconnect(impl_->session, "shell failed");
          libssh2_session_free(impl_->session); impl_->session = nullptr;
          ::close(impl_->sock); impl_->sock = -1;
          libssh2_exit(); return;
      }
    }

    connected_.store(true);

    { GuacInstruction s; s.opcode="size";
      s.args={std::to_string(stream_id_),std::to_string(cols),std::to_string(rows)};
      callback_(s); }
    { GuacInstruction p; p.opcode="pipe";
      p.args={std::to_string(stream_id_),"terminal","text/plain"};
      callback_(p); }

    run_io_loop(impl_->sock, impl_->session, ch,
                impl_->stop, input_queue_mutex_, input_queue_, connected_,
                [this](const char* d, size_t l){ flush_terminal_output(d, l); });

    { GuacInstruction e; e.opcode="end";
      e.args={std::to_string(stream_id_)}; callback_(e); }

    libssh2_channel_send_eof(ch);
    libssh2_channel_close(ch);
    libssh2_channel_free(ch);

    libssh2_session_disconnect(impl_->session, "Normal Shutdown");
    libssh2_session_free(impl_->session); impl_->session = nullptr;
    ::close(impl_->sock); impl_->sock = -1;
    libssh2_exit();
}

} // namespace proxy
