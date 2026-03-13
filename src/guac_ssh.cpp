#include "core/guac_ssh.h"

#include <libssh2.h>

#include <openssl/evp.h>

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include <stdexcept>
#include <cstring>
#include <vector>

namespace proxy {

// ── pImpl ────────────────────────────────────────────────────────────────────

struct GuacSshClient::Impl {
    int          sock{-1};
    LIBSSH2_SESSION* session{nullptr};
    LIBSSH2_CHANNEL* channel{nullptr};
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
    if (impl_) {
        impl_->stop = true;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool GuacSshClient::is_connected() const {
    return connected_.load();
}

void GuacSshClient::send_input(const std::string& data) {
    if (!connected_.load() || data.empty()) return;
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (impl_->channel) {
        size_t total = 0;
        while (total < data.size()) {
            ssize_t n = libssh2_channel_write(
                impl_->channel,
                data.data() + total,
                data.size()  - total);
            if (n == LIBSSH2_ERROR_EAGAIN) continue;
            if (n < 0) break;
            total += static_cast<size_t>(n);
        }
    }
}

// ── base64 인코딩 (OpenSSL EVP) ───────────────────────────────────────────────

static std::string base64_encode(const unsigned char* data, size_t len) {
    if (len == 0) return {};
    // EVP_EncodeBlock 출력 크기: ceil(len/3)*4 + 1 (null terminator)
    std::vector<unsigned char> buf(((len + 2) / 3) * 4 + 1);
    int out_len = EVP_EncodeBlock(buf.data(), data, static_cast<int>(len));
    return std::string(reinterpret_cast<char*>(buf.data()),
                       static_cast<size_t>(out_len));
}

// ── flush_terminal_output ─────────────────────────────────────────────────────

void GuacSshClient::flush_terminal_output(const char* data, size_t len) {
    // blob 하나당 최대 8KB의 원본 데이터 (base64 후 ~10.7KB < 16KB 상한)
    constexpr size_t CHUNK = 8192;

    for (size_t offset = 0; offset < len; offset += CHUNK) {
        size_t chunk_len = std::min(CHUNK, len - offset);
        std::string b64 = base64_encode(
            reinterpret_cast<const unsigned char*>(data + offset),
            chunk_len);

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
    int err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0) {
        throw std::runtime_error(
            std::string("getaddrinfo: ") + gai_strerror(err));
    }

    int sock = -1;
    for (addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
        sock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (::connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        ::close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0) {
        throw std::runtime_error("tcp_connect: cannot connect to " +
                                 host + ":" + port_str);
    }
    return sock;
}

// ── run_event_loop ─────────────────────────────────────────────────────────────

void GuacSshClient::run_event_loop(const std::string& host, uint16_t port,
                                   const std::string& username,
                                   const std::string& password,
                                   uint16_t cols, uint16_t rows)
{
    // ── 1. libssh2 초기화 ────────────────────────────────────────────────────
    if (libssh2_init(0) != 0) {
        return;
    }

    // ── 2. TCP 연결 ──────────────────────────────────────────────────────────
    try {
        impl_->sock = tcp_connect(host, port);
    } catch (...) {
        libssh2_exit();
        return;
    }

    // ── 3. SSH 세션 초기화 + 핸드셰이크 ──────────────────────────────────────
    impl_->session = libssh2_session_init();
    if (!impl_->session) {
        ::close(impl_->sock);
        impl_->sock = -1;
        libssh2_exit();
        return;
    }
    // 블로킹 모드로 설정 (read 루프에서 직접 select 없이 사용)
    libssh2_session_set_blocking(impl_->session, 1);

    if (libssh2_session_handshake(impl_->session, impl_->sock) != 0) {
        libssh2_session_free(impl_->session);
        impl_->session = nullptr;
        ::close(impl_->sock);
        impl_->sock = -1;
        libssh2_exit();
        return;
    }

    // ── 4. 비밀번호 인증 ─────────────────────────────────────────────────────
    if (libssh2_userauth_password(impl_->session,
                                  username.c_str(),
                                  password.c_str()) != 0) {
        libssh2_session_disconnect(impl_->session, "auth failed");
        libssh2_session_free(impl_->session);
        impl_->session = nullptr;
        ::close(impl_->sock);
        impl_->sock = -1;
        libssh2_exit();
        return;
    }

    // ── 5. 세션 채널 오픈 ────────────────────────────────────────────────────
    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(impl_->session);
    if (!ch) {
        libssh2_session_disconnect(impl_->session, "channel open failed");
        libssh2_session_free(impl_->session);
        impl_->session = nullptr;
        ::close(impl_->sock);
        impl_->sock = -1;
        libssh2_exit();
        return;
    }

    // ── 6. PTY 요청 (xterm, cols×rows) ───────────────────────────────────────
    // libssh2_channel_request_pty_ex(channel, term, term_len,
    //                                modes, modes_len, width, height,
    //                                pixel_w, pixel_h)
    if (libssh2_channel_request_pty_ex(
            ch,
            "xterm", 5,     // 터미널 타입 "xterm"
            nullptr, 0,     // 터미널 모드 (기본값)
            cols, rows,
            0, 0) != 0)
    {
        libssh2_channel_free(ch);
        libssh2_session_disconnect(impl_->session, "pty failed");
        libssh2_session_free(impl_->session);
        impl_->session = nullptr;
        ::close(impl_->sock);
        impl_->sock = -1;
        libssh2_exit();
        return;
    }

    // ── 7. 셸 시작 ───────────────────────────────────────────────────────────
    if (libssh2_channel_shell(ch) != 0) {
        libssh2_channel_free(ch);
        libssh2_session_disconnect(impl_->session, "shell failed");
        libssh2_session_free(impl_->session);
        impl_->session = nullptr;
        ::close(impl_->sock);
        impl_->sock = -1;
        libssh2_exit();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        impl_->channel = ch;
    }
    connected_.store(true);

    // ── 8. Guacamole 연결 수립 명령어 ────────────────────────────────────────
    // size: 터미널 크기 (스트림 ID = stream_id_)
    {
        GuacInstruction size;
        size.opcode = "size";
        size.args   = { std::to_string(stream_id_),
                        std::to_string(cols),
                        std::to_string(rows) };
        callback_(size);
    }
    // pipe: 터미널 데이터 스트림 오픈
    {
        GuacInstruction pipe;
        pipe.opcode = "pipe";
        pipe.args   = { std::to_string(stream_id_),
                        "terminal",
                        "text/plain" };
        callback_(pipe);
    }

    // ── 9. 터미널 출력 읽기 루프 ─────────────────────────────────────────────
    char buf[4096];
    while (!impl_->stop.load()) {
        ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf));
        if (n == LIBSSH2_ERROR_EAGAIN) {
            // 논블로킹 모드에서만 발생. 블로킹 모드이므로 사실상 도달 안 함.
            continue;
        }
        if (n <= 0) {
            // EOF 또는 오류 → 루프 종료
            break;
        }
        flush_terminal_output(buf, static_cast<size_t>(n));
    }

    // ── 10. 종료 — end 명령어 + libssh2 정리 ─────────────────────────────────
    connected_.store(false);

    {
        GuacInstruction end;
        end.opcode = "end";
        end.args   = { std::to_string(stream_id_) };
        callback_(end);
    }

    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        libssh2_channel_send_eof(ch);
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
        impl_->channel = nullptr;
    }

    libssh2_session_disconnect(impl_->session, "Normal Shutdown");
    libssh2_session_free(impl_->session);
    impl_->session = nullptr;

    ::close(impl_->sock);
    impl_->sock = -1;

    libssh2_exit();
}

// ── run_event_loop_key ─────────────────────────────────────────────────────────
// connect()와 동일하나 인증을 libssh2_userauth_publickey_fromfile()로 수행한다.

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
    libssh2_session_set_blocking(impl_->session, 1);

    if (libssh2_session_handshake(impl_->session, impl_->sock) != 0) {
        libssh2_session_free(impl_->session); impl_->session = nullptr;
        ::close(impl_->sock); impl_->sock = -1;
        libssh2_exit(); return;
    }

    std::string pubkey_path = privkey_path + ".pub";
    if (libssh2_userauth_publickey_fromfile(
            impl_->session,
            username.c_str(),
            pubkey_path.c_str(),
            privkey_path.c_str(),
            nullptr) != 0)
    {
        libssh2_session_disconnect(impl_->session, "auth failed");
        libssh2_session_free(impl_->session); impl_->session = nullptr;
        ::close(impl_->sock); impl_->sock = -1;
        libssh2_exit(); return;
    }

    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(impl_->session);
    if (!ch) {
        libssh2_session_disconnect(impl_->session, "channel open failed");
        libssh2_session_free(impl_->session); impl_->session = nullptr;
        ::close(impl_->sock); impl_->sock = -1;
        libssh2_exit(); return;
    }

    if (libssh2_channel_request_pty_ex(ch, "xterm", 5, nullptr, 0,
                                        cols, rows, 0, 0) != 0) {
        libssh2_channel_free(ch);
        libssh2_session_disconnect(impl_->session, "pty failed");
        libssh2_session_free(impl_->session); impl_->session = nullptr;
        ::close(impl_->sock); impl_->sock = -1;
        libssh2_exit(); return;
    }

    if (libssh2_channel_shell(ch) != 0) {
        libssh2_channel_free(ch);
        libssh2_session_disconnect(impl_->session, "shell failed");
        libssh2_session_free(impl_->session); impl_->session = nullptr;
        ::close(impl_->sock); impl_->sock = -1;
        libssh2_exit(); return;
    }

    { std::lock_guard<std::mutex> lock(channel_mutex_); impl_->channel = ch; }
    connected_.store(true);

    { GuacInstruction s; s.opcode="size";
      s.args={std::to_string(stream_id_),std::to_string(cols),std::to_string(rows)};
      callback_(s); }
    { GuacInstruction p; p.opcode="pipe";
      p.args={std::to_string(stream_id_),"terminal","text/plain"};
      callback_(p); }

    char buf[4096];
    while (!impl_->stop.load()) {
        ssize_t n = libssh2_channel_read(ch, buf, sizeof(buf));
        if (n == LIBSSH2_ERROR_EAGAIN) continue;
        if (n <= 0) break;
        flush_terminal_output(buf, static_cast<size_t>(n));
    }

    connected_.store(false);
    { GuacInstruction e; e.opcode="end"; e.args={std::to_string(stream_id_)}; callback_(e); }

    { std::lock_guard<std::mutex> lock(channel_mutex_);
      libssh2_channel_send_eof(ch);
      libssh2_channel_close(ch);
      libssh2_channel_free(ch);
      impl_->channel = nullptr; }

    libssh2_session_disconnect(impl_->session, "Normal Shutdown");
    libssh2_session_free(impl_->session); impl_->session = nullptr;
    ::close(impl_->sock); impl_->sock = -1;
    libssh2_exit();
}

} // namespace proxy
