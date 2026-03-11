#include "core/tls_proxy.h"
#include "utils/logger.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/err.h>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <cerrno>

// ── 생성자 / 소멸자 ────────────────────────────────────────────────────────────

TlsProxy::TlsProxy(int local_port, const std::string& target_ip, int target_port,
                   const std::string& cert_file, const std::string& key_file,
                   int max_events)
    : ssl_ctx_(nullptr)
    , listen_fd_(-1)
    , epoll_fd_(-1)
    , target_ip_(target_ip)
    , target_port_(target_port)
    , max_events_(max_events)
    , running_(false)
{
    // OpenSSL 전역 초기화: 알고리즘 테이블 + 에러 문자열 로드
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                     nullptr);

    init_ssl_context(cert_file, key_file);

    // epoll 인스턴스 생성
    // EPOLL_CLOEXEC: exec() 계열 호출 시 자식 프로세스에 fd 누출 방지
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        SSL_CTX_free(ssl_ctx_);
        throw std::runtime_error("epoll_create1: " + std::string(strerror(errno)));
    }

    // 리스닝 소켓 생성
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        close(epoll_fd_);
        SSL_CTX_free(ssl_ctx_);
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(local_port));

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_); close(epoll_fd_); SSL_CTX_free(ssl_ctx_);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }

    if (listen(listen_fd_, SOMAXCONN) < 0) {
        close(listen_fd_); close(epoll_fd_); SSL_CTX_free(ssl_ctx_);
        throw std::runtime_error("listen: " + std::string(strerror(errno)));
    }

    // ET 모드에서는 listen_fd도 논블로킹이어야 한다.
    // 블로킹 소켓에서 accept()를 반복하면 대기 연결이 없을 때 이벤트 루프 전체가 멈춘다.
    set_nonblocking(listen_fd_);

    // listen_fd를 epoll에 등록해 새 연결 이벤트를 감지한다.
    add_to_epoll(listen_fd_, EPOLLIN | EPOLLET);

    Logger::info("[TlsProxy] initialized on port " + std::to_string(local_port));
}

TlsProxy::~TlsProxy() {
    stop();

    // 핸드셰이크 진행 중인 연결 정리
    for (auto& [fd, ssl] : pending_ssl_) {
        remove_from_epoll(fd);
        SSL_free(ssl);
        close(fd);
    }
    pending_ssl_.clear();

    // 완료된 연결 정리
    // connections_는 client_fd/target_fd 양쪽 다 키로 등록되어 있어
    // SSL_free를 중복 호출하지 않기 위해 is_client=true 쪽만 SSL 해제한다.
    for (auto& [fd, state] : connections_) {
        remove_from_epoll(fd);
        if (state.is_client && state.ssl) {
            SSL_free(state.ssl);
        }
        close(fd);
    }
    connections_.clear();

    if (epoll_fd_  >= 0) { close(epoll_fd_);  epoll_fd_  = -1; }
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    if (ssl_ctx_)        { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
}

// ── 메인 이벤트 루프 ───────────────────────────────────────────────────────────

void TlsProxy::run() {
    running_ = true;

    std::vector<epoll_event> events(max_events_);
    Logger::info("[TlsProxy] event loop started");

    while (running_) {
        int n = epoll_wait(epoll_fd_, events.data(), max_events_, -1);

        if (n < 0) {
            if (errno == EINTR) continue;  // 시그널로 중단됨 → 재시도
            Logger::error("[TlsProxy] epoll_wait: " + std::string(strerror(errno)));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // 새 TCP 연결 — accept + SSL 객체 생성 후 핸드셰이크 시작
                accept_connection();
            } else if (pending_ssl_.count(fd)) {
                // SSL_accept() 진행 중 — 재시도
                continue_handshake(fd);
            } else {
                // 핸드셰이크 완료된 연결 — 데이터 포워딩
                handle_event(fd, events[i].events);
            }
        }
    }

    Logger::info("[TlsProxy] event loop stopped");
    running_ = false;
}

void TlsProxy::stop() {
    running_ = false;
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
}

// ── SSL 초기화 (Phase 4-A에서 구현) ───────────────────────────────────────────

void TlsProxy::init_ssl_context(const std::string& cert_file, const std::string& key_file) {
    // TLS_server_method(): TLS 1.2/1.3 자동 협상 서버 메서드
    ssl_ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ssl_ctx_) {
        throw std::runtime_error("SSL_CTX_new: " + ssl_error_string());
    }

    // TLS 1.0/1.1은 BEAST, POODLE 등 알려진 취약점 → 최소 1.2 강제
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);

    // 서버 인증서 로드 (PEM 형식)
    if (SSL_CTX_use_certificate_file(ssl_ctx_, cert_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr;
        throw std::runtime_error("SSL_CTX_use_certificate_file(" + cert_file + "): "
                                 + ssl_error_string());
    }

    // 서버 개인키 로드
    if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr;
        throw std::runtime_error("SSL_CTX_use_PrivateKey_file(" + key_file + "): "
                                 + ssl_error_string());
    }

    // 인증서와 개인키가 실제 쌍인지 검증 — 혼용 실수를 시작 시점에 잡아냄
    if (SSL_CTX_check_private_key(ssl_ctx_) != 1) {
        SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr;
        throw std::runtime_error("SSL_CTX_check_private_key: " + ssl_error_string());
    }

    Logger::info("[TlsProxy] SSL_CTX initialized (TLS 1.2+), cert: " + cert_file);
}

std::string TlsProxy::ssl_error_string() {
    unsigned long err = ERR_get_error();
    if (err == 0) return "unknown error";
    const char* reason = ERR_reason_error_string(err);
    return reason ? std::string(reason) : "error code " + std::to_string(err);
}

// ── 소켓 유틸리티 ─────────────────────────────────────────────────────────────

void TlsProxy::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::runtime_error("fcntl(F_GETFL): " + std::string(strerror(errno)));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("fcntl(F_SETFL, O_NONBLOCK): " + std::string(strerror(errno)));
    }
}

int TlsProxy::connect_to_target() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket: " + std::string(strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(target_port_));
    if (inet_pton(AF_INET, target_ip_.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        throw std::runtime_error("inet_pton: invalid address " + target_ip_);
    }

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("connect to " + target_ip_ + ":"
                                 + std::to_string(target_port_)
                                 + " failed: " + strerror(errno));
    }

    set_nonblocking(fd);
    return fd;
}

// ── epoll 관리 ────────────────────────────────────────────────────────────────

void TlsProxy::add_to_epoll(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::runtime_error("epoll_ctl(ADD): " + std::string(strerror(errno)));
    }
}

void TlsProxy::mod_epoll(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        throw std::runtime_error("epoll_ctl(MOD): " + std::string(strerror(errno)));
    }
}

void TlsProxy::remove_from_epoll(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

// ── 연결 수락 ─────────────────────────────────────────────────────────────────

void TlsProxy::accept_connection() {
    // ET 모드: EAGAIN까지 반복 accept()
    while (true) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            Logger::error("[TlsProxy] accept: " + std::string(strerror(errno)));
            break;
        }

        set_nonblocking(client_fd);

        // SSL 객체 생성 — 이 연결 전용 TLS 상태
        SSL* ssl = SSL_new(ssl_ctx_);
        if (!ssl) {
            Logger::error("[TlsProxy] SSL_new failed: " + ssl_error_string());
            close(client_fd);
            continue;
        }

        // SSL 객체와 TCP 소켓을 연결
        // SSL_set_fd()는 내부적으로 BIO(I/O 추상화 레이어)를 설정한다.
        // 이후 SSL_accept/SSL_read/SSL_write는 이 fd를 통해 읽고 쓴다.
        SSL_set_fd(ssl, client_fd);

        // pending_ssl_에 등록 — 핸드셰이크는 continue_handshake()에서 처리
        pending_ssl_[client_fd] = ssl;

        // EPOLLIN | EPOLLET: 클라이언트 ClientHello 수신 이벤트 감지
        add_to_epoll(client_fd, EPOLLIN | EPOLLET);

        Logger::debug("[TlsProxy] new TCP connection, starting TLS handshake (fd="
                      + std::to_string(client_fd) + ")");
    }
}

// ── TLS 핸드셰이크 ────────────────────────────────────────────────────────────

void TlsProxy::continue_handshake(int fd) {
    SSL* ssl = pending_ssl_[fd];

    int ret = SSL_accept(ssl);

    if (ret == 1) {
        // 핸드셰이크 성공 — 타겟 서버에 연결
        int target_fd = -1;
        try {
            target_fd = connect_to_target();
        } catch (const std::exception& e) {
            Logger::error("[TlsProxy] connect_to_target after handshake: "
                          + std::string(e.what()));
            close_pending(fd);
            return;
        }

        // pending_ssl_에서 제거하고 connections_로 이동
        pending_ssl_.erase(fd);

        // 양방향 매핑 등록
        //   connections_[client_fd]: SSL 객체 보관 (포워딩 시 참조)
        //   connections_[target_fd]: SSL 없음 (평문 TCP)
        connections_[fd]        = { target_fd, true,  ssl,     {} };
        connections_[target_fd] = { fd,        false, nullptr, {} };

        // target_fd도 epoll에 등록
        add_to_epoll(target_fd, EPOLLIN | EPOLLET);

        Logger::info("[TlsProxy] TLS handshake OK (client_fd=" + std::to_string(fd)
                     + " target_fd=" + std::to_string(target_fd) + ")");
        return;
    }

    int err = SSL_get_error(ssl, ret);

    if (err == SSL_ERROR_WANT_READ) {
        // 클라이언트 데이터 더 필요 — EPOLLIN 대기 (기존 설정 유지)
        return;
    }
    if (err == SSL_ERROR_WANT_WRITE) {
        // 쓰기 버퍼가 가득 참 — EPOLLOUT으로 변경해 쓰기 가능 시 재시도
        mod_epoll(fd, EPOLLOUT | EPOLLET);
        return;
    }

    // 핸드셰이크 실패 (인증서 불일치, 프로토콜 오류 등)
    Logger::error("[TlsProxy] SSL_accept failed (fd=" + std::to_string(fd)
                  + "): " + ssl_error_string());
    close_pending(fd);
}

// ── 데이터 포워딩 ─────────────────────────────────────────────────────────────

void TlsProxy::handle_event(int fd, uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        close_connection(fd);
        return;
    }

    // EPOLLOUT: 이전에 EAGAIN/WANT_WRITE로 막혔던 쓰기 버퍼를 재전송
    if (events & EPOLLOUT) {
        flush_write_buf(fd);
        // flush 중 연결이 끊겼을 수 있으므로 재확인
        if (connections_.find(fd) == connections_.end()) return;
    }

    if (!(events & EPOLLIN)) return;

    auto it = connections_.find(fd);
    if (it == connections_.end()) return;  // 이미 정리된 fd

    const ConnState& state = it->second;

    if (state.is_client) {
        // 클라이언트 fd에 이벤트: TLS → 평문 포워딩
        forward_client_to_target(fd, state.ssl, state.peer_fd);
    } else {
        // 타겟 fd에 이벤트: 평문 → TLS 포워딩
        // 클라이언트 fd의 SSL 객체를 connections_[peer_fd]에서 가져온다
        auto client_it = connections_.find(state.peer_fd);
        if (client_it == connections_.end()) {
            close_connection(fd);
            return;
        }
        forward_target_to_client(fd, state.peer_fd, client_it->second.ssl);
    }
}

void TlsProxy::forward_client_to_target(int client_fd, SSL* ssl, int target_fd) {
    // SSL_read 버퍼 크기: 16KB
    // TLS 레코드 최대 크기는 16KB (RFC 5246, Section 6.2.1).
    // 버퍼를 이 크기에 맞추면 레코드 단위로 처리해 불필요한 분할이 없다.
    char buf[16384];

    while (true) {
        int n = SSL_read(ssl, buf, sizeof(buf));

        if (n > 0) {
            // 읽은 데이터를 타겟에 전송
            // write()는 partial write가 가능하므로 루프로 처리
            ssize_t total = 0;
            while (total < static_cast<ssize_t>(n)) {
                ssize_t w = write(target_fd, buf + total, n - total);
                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // 타겟 쓰기 버퍼 가득 참 — 미전송 데이터를 target_fd의 write_buf에 보관
                        // EPOLLOUT 이벤트가 오면 flush_write_buf()가 재전송한다
                        auto& tbuf = connections_[target_fd].write_buf;
                        tbuf.insert(tbuf.end(), buf + total, buf + n);
                        mod_epoll(target_fd, EPOLLIN | EPOLLOUT | EPOLLET);
                        return;
                    }
                    close_connection(client_fd);
                    return;
                }
                if (w == 0) {
                    close_connection(client_fd);
                    return;
                }
                total += w;
            }
            continue;
        }

        int err = SSL_get_error(ssl, n);

        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // 읽을 데이터 없음 — epoll이 다음 이벤트를 알려줄 때까지 대기
            break;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            // TLS close_notify: 클라이언트가 TLS 세션을 정상 종료
            Logger::debug("[TlsProxy] client TLS close_notify (fd=" + std::to_string(client_fd) + ")");
        } else {
            Logger::debug("[TlsProxy] SSL_read error (fd=" + std::to_string(client_fd)
                          + "): " + ssl_error_string());
        }
        close_connection(client_fd);
        return;
    }
}

void TlsProxy::forward_target_to_client(int target_fd, int client_fd, SSL* ssl) {
    char buf[16384];

    while (true) {
        ssize_t n = read(target_fd, buf, sizeof(buf));

        if (n > 0) {
            // 읽은 데이터를 TLS로 암호화해 클라이언트에 전송
            // SSL_write()는 partial write 없이 n바이트 전부를 처리하거나 에러를 반환한다.
            // (SSL 레코드 단위로 묶어서 쓰기 때문)
            int w = SSL_write(ssl, buf, static_cast<int>(n));
            if (w <= 0) {
                int err = SSL_get_error(ssl, w);
                if (err == SSL_ERROR_WANT_WRITE) {
                    // 클라이언트 쓰기 버퍼 가득 참 — 미전송 데이터를 client_fd의 write_buf에 보관
                    // EPOLLOUT 이벤트가 오면 flush_write_buf()가 SSL_write로 재전송한다
                    auto& cbuf = connections_[client_fd].write_buf;
                    cbuf.insert(cbuf.end(), buf, buf + n);
                    mod_epoll(client_fd, EPOLLIN | EPOLLOUT | EPOLLET);
                    return;
                }
                if (err == SSL_ERROR_WANT_READ) {
                    // TLS 재협상(renegotiation) — 클라이언트에서 읽어야 진행됨
                    // EPOLLIN은 이미 등록돼 있으므로 그대로 대기
                    return;
                }
                close_connection(target_fd);
                return;
            }
            continue;
        }

        if (n == 0) {
            // EOF: 타겟 서버가 연결을 정상 종료
            Logger::debug("[TlsProxy] target EOF (fd=" + std::to_string(target_fd) + ")");
            close_connection(target_fd);
            return;
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 읽을 데이터 없음 — epoll 대기
            break;
        }
        close_connection(target_fd);
        return;
    }
}

// ── 쓰기 버퍼 플러시 ─────────────────────────────────────────────────────────

void TlsProxy::flush_write_buf(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    ConnState& state = it->second;
    auto& buf = state.write_buf;
    if (buf.empty()) {
        // 버퍼가 이미 비어있으면 EPOLLOUT만 해제
        mod_epoll(fd, EPOLLIN | EPOLLET);
        return;
    }

    if (!state.is_client) {
        // target_fd: 평문 write() 재전송
        ssize_t written = 0;
        while (written < static_cast<ssize_t>(buf.size())) {
            ssize_t w = write(fd, buf.data() + written, buf.size() - written);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 아직 버퍼 여유 없음 — 전송된 부분 제거 후 EPOLLOUT 유지
                    buf.erase(buf.begin(), buf.begin() + written);
                    return;
                }
                close_connection(fd);
                return;
            }
            if (w == 0) { close_connection(fd); return; }
            written += w;
        }
        // 전체 전송 완료 — 버퍼 비우고 EPOLLOUT 해제
        buf.clear();
        mod_epoll(fd, EPOLLIN | EPOLLET);

    } else {
        // client_fd: SSL_write() 재전송
        // SSL_write는 이전에 실패한 것과 동일한 포인터/크기로 재호출해야 한다.
        // (OpenSSL 내부 상태가 이전 호출을 기억하기 때문)
        // write_buf 전체를 한 번에 재시도한다.
        SSL* ssl = state.ssl;
        int w = SSL_write(ssl, buf.data(), static_cast<int>(buf.size()));
        if (w <= 0) {
            int err = SSL_get_error(ssl, w);
            if (err == SSL_ERROR_WANT_WRITE) {
                // 아직 버퍼 여유 없음 — EPOLLOUT 유지
                return;
            }
            close_connection(fd);
            return;
        }
        // 전체 전송 완료 — 버퍼 비우고 EPOLLOUT 해제
        buf.clear();
        mod_epoll(fd, EPOLLIN | EPOLLET);
    }
}

// ── 연결 정리 ─────────────────────────────────────────────────────────────────

void TlsProxy::close_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    int  peer_fd   = it->second.peer_fd;
    bool is_client = it->second.is_client;

    // SSL 객체는 is_client=true 쪽(client_fd 엔트리)에 저장되어 있다.
    // 어느 쪽 fd로 close_connection이 호출되든 동일한 SSL 객체를 가리켜야 한다.
    SSL* ssl = nullptr;
    if (is_client) {
        ssl = it->second.ssl;
    } else {
        auto peer_it = connections_.find(peer_fd);
        if (peer_it != connections_.end()) {
            ssl = peer_it->second.ssl;
        }
    }

    remove_from_epoll(fd);
    remove_from_epoll(peer_fd);

    // SSL_shutdown: TLS close_notify alert 전송 후 소켓 닫기
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }

    close(fd);
    close(peer_fd);

    connections_.erase(fd);
    connections_.erase(peer_fd);

    Logger::debug("[TlsProxy] connection closed (fd=" + std::to_string(fd)
                  + " peer=" + std::to_string(peer_fd) + ")");
}

void TlsProxy::close_pending(int fd) {
    auto it = pending_ssl_.find(fd);
    if (it == pending_ssl_.end()) return;

    SSL* ssl = it->second;
    remove_from_epoll(fd);
    SSL_free(ssl);
    close(fd);
    pending_ssl_.erase(it);
}
