#include "core/tls_proxy.h"
#include "utils/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <openssl/err.h>
#include <stdexcept>
#include <cstring>

TlsProxy::TlsProxy(int local_port, const std::string& target_ip, int target_port,
                   const std::string& cert_file, const std::string& key_file)
    : ssl_ctx_(nullptr)
    , listen_fd_(-1)
    , target_ip_(target_ip)
    , target_port_(target_port)
    , running_(false)
{
    // OpenSSL 전역 초기화: 알고리즘 테이블 + 에러 문자열 로드
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                     nullptr);

    init_ssl_context(cert_file, key_file);

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
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
        close(listen_fd_);
        SSL_CTX_free(ssl_ctx_);
        throw std::runtime_error("bind: " + std::string(strerror(errno)));
    }

    if (listen(listen_fd_, SOMAXCONN) < 0) {
        close(listen_fd_);
        SSL_CTX_free(ssl_ctx_);
        throw std::runtime_error("listen: " + std::string(strerror(errno)));
    }

    Logger::info("[TlsProxy] initialized on port " + std::to_string(local_port));
}

TlsProxy::~TlsProxy() {
    stop();
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    // SSL_CTX_free: 참조 카운트 감소 → 0이면 해제
    if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
}

void TlsProxy::run() {
    // TODO Phase 4-B: epoll 루프 + SSL_accept 핸드셰이크
    running_ = true;
    Logger::info("[TlsProxy] run() — Phase 4-B에서 구현 예정");
}

void TlsProxy::stop() {
    running_ = false;
    if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
}

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
