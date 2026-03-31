/**
 * @file guac_http.cpp
 * @brief Phase 14-A — libcurl HTTP GET + response Guacamole instruction
 *
 * ── libcurl 응답 수집 ─────────────────────────────────────────────────────
 *
 *   CURLOPT_WRITEFUNCTION + CURLOPT_WRITEDATA로 응답 바디를 std::string에 누적한다.
 *   CURLOPT_HEADERFUNCTION으로 Content-Type 헤더를 추출한다.
 *
 * ── base64 인코딩 ─────────────────────────────────────────────────────────
 *
 *   OpenSSL EVP_EncodeBlock을 사용한다 (이미 링크된 의존성이므로 추가 라이브러리 불필요).
 *
 * ── response instruction ──────────────────────────────────────────────────
 *
 *   형식: response,<status_code>,<content_type>,<base64_body>;
 *   GuacParser::serialize()로 직렬화해 콜백에 전달한다.
 */

#include "core/guac_http.h"

#include <curl/curl.h>
#include <openssl/evp.h>    // EVP_EncodeBlock

#include <stdexcept>
#include <cstring>

namespace proxy {

// ── Impl ──────────────────────────────────────────────────────────────────

struct GuacHttpClient::Impl {
    // 응답 수집 버퍼
    std::string body;
    std::string content_type;
    long        status_code{0};
};

// ── libcurl 콜백 ──────────────────────────────────────────────────────────

static size_t write_body(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t write_header(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ct = static_cast<std::string*>(userdata);
    std::string line(static_cast<char*>(ptr), size * nmemb);
    // "Content-Type: text/html; charset=utf-8\r\n"
    const std::string prefix = "content-type:";
    std::string lower = line;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (lower.find(prefix) == 0) {
        size_t start = prefix.size();
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) start++;
        size_t end = line.find_first_of("\r\n", start);
        *ct = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    return size * nmemb;
}

// ── base64 인코딩 ─────────────────────────────────────────────────────────

static std::string base64_encode(const std::string& data) {
    if (data.empty()) return "";
    size_t out_len = 4 * ((data.size() + 2) / 3) + 1;
    std::string result(out_len, '\0');
    int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()),
        reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<int>(data.size())
    );
    result.resize(static_cast<size_t>(written));
    return result;
}

// ── GuacHttpClient 구현 ───────────────────────────────────────────────────

GuacHttpClient::GuacHttpClient(InstructionCallback callback)
    : impl_(std::make_unique<Impl>())
    , callback_(std::move(callback))
{}

GuacHttpClient::~GuacHttpClient() {
    disconnect();
}

bool GuacHttpClient::is_connected() const {
    return connected_.load();
}

void GuacHttpClient::connect(const std::string& url) {
    if (connected_.load()) return;
    abort_     = false;
    connected_ = true;
    worker_    = std::thread(&GuacHttpClient::run_request, this, url);
}

void GuacHttpClient::disconnect() {
    abort_ = true;
    if (worker_.joinable()) worker_.join();
    connected_ = false;
}

void GuacHttpClient::run_request(const std::string& url) {
    impl_->body.clear();
    impl_->content_type.clear();
    impl_->status_code = 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        // 초기화 실패 → error instruction
        GuacInstruction err;
        err.opcode = "error";
        err.args   = {"curl_easy_init failed", "500"};
        if (callback_) callback_(err);
        connected_ = false;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    // SSL 검증 — 자체 서명 인증서를 허용하려면 CURLOPT_SSL_VERIFYPEER를 0으로 설정
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // 응답 바디 수집
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &impl_->body);
    // Content-Type 헤더 수집
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     &impl_->content_type);

    CURLcode res = curl_easy_perform(curl);

    if (!abort_.load()) {
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &impl_->status_code);

            std::string ct = impl_->content_type.empty() ? "application/octet-stream"
                                                          : impl_->content_type;
            std::string b64 = base64_encode(impl_->body);

            GuacInstruction instr;
            instr.opcode = "response";
            instr.args   = {
                std::to_string(impl_->status_code),
                ct,
                b64
            };
            if (callback_) callback_(instr);
        } else {
            // HTTP 요청 실패 → error instruction
            GuacInstruction err;
            err.opcode = "error";
            err.args   = {curl_easy_strerror(res), "502"};
            if (callback_) callback_(err);
        }
    }

    curl_easy_cleanup(curl);
    connected_ = false;
}

} // namespace proxy
