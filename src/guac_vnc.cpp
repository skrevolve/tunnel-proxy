/**
 * @file guac_vnc.cpp
 * @brief Phase 8-D — libvncclient 기반 VNC 클라이언트 구현 + Guacamole 화면 스트리밍
 *
 * ── 구현 개요 ─────────────────────────────────────────────────────────────
 *
 *   libvncclient를 사용해 VNC 서버에 연결한다.
 *   GotFrameBufferUpdate 콜백에서 더티 영역을 BGRX → RGBA 변환 후
 *   PNG(zlib DEFLATE)로 인코딩하고 OpenSSL base64로 직렬화해
 *   Guacamole img/blob/end 명령어로 내보낸다.
 *
 * ── 픽셀 포맷 ─────────────────────────────────────────────────────────────
 *
 *   rfbGetClient(8, 3, 4): bitsPerPixel=32, depth=24
 *   리틀엔디안에서 메모리 순서: B(0) G(1) R(2) X(3) = BGRX
 *   PNG colortype 6(RGBA)에 맞게 flush_dirty_region에서 채널 스왑 수행.
 *
 * ── Callbacks 중첩 구조체 (friend) ─────────────────────────────────────────
 *
 *   GuacVncClient::Callbacks는 GuacVncClient의 friend이므로 private 멤버에
 *   직접 접근 가능하다. libvncclient C 콜백을 C++ static 메서드로 구현하면
 *   C 함수 포인터 호환성을 유지하면서 클래스 private 멤버를 사용할 수 있다.
 *   GuacVncClient* 복원: rfbClientGetClientData(client, TAG)
 *
 * ── PNG 인코딩 (inline, zlib만 사용) ──────────────────────────────────────
 *
 *   Phase 8-B(guac_rdp.cpp)와 동일한 방식으로 zlib compress2() + crc32() 사용.
 *   외부 PNG 라이브러리 불필요.
 */

// ── libvncclient 헤더 (guac_vnc.h에는 노출되지 않음) ──────────────────────
#include <rfb/rfbclient.h>

// ── 표준 / 프로젝트 헤더 ──────────────────────────────────────────────────
#include "core/guac_vnc.h"
#include "core/guac_parser.h"

#include <openssl/evp.h>        // EVP_EncodeBlock (base64)
#include <zlib.h>               // compress2, crc32, compressBound

#include <stdexcept>
#include <cstring>
#include <vector>
#include <arpa/inet.h>          // htonl

namespace proxy {

// ── rfbClientGetClientData/SetClientData 태그 ────────────────────────────

// rfbClientSetClientData/GetClientData는 void* 태그로 데이터 슬롯을 구분한다.
// 충돌 방지를 위해 이 번역 단위 전용 태그를 하나 선언한다.
static int kClientDataTag = 0;

// ── GuacVncClient::Impl ───────────────────────────────────────────────────

struct GuacVncClient::Impl {
    rfbClient*        rfb{nullptr};
    std::atomic<bool> stop{false};
};

// ── PNG 인코딩 헬퍼 (Phase 8-B guac_rdp.cpp와 동일한 구조) ───────────────

static void write_png_chunk(std::vector<uint8_t>& out,
                            const char type[4],
                            const uint8_t* data, uint32_t len)
{
    auto push_be32 = [&](uint32_t v) {
        out.push_back((v >> 24) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >>  8) & 0xFF);
        out.push_back( v        & 0xFF);
    };
    push_be32(len);
    out.insert(out.end(), type, type + 4);
    if (len > 0 && data) out.insert(out.end(), data, data + len);

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(type), 4);
    if (len > 0 && data) crc = crc32(crc, data, len);
    push_be32(static_cast<uint32_t>(crc));
}

static std::vector<uint8_t> encode_png_rgba(
        const uint8_t* rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0) return {};

    static const uint8_t PNG_SIG[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };

    std::vector<uint8_t> out;
    out.insert(out.end(), PNG_SIG, PNG_SIG + 8);

    uint8_t ihdr[13];
    auto be32 = [](uint8_t* p, uint32_t v) {
        p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
        p[2] = (v >>  8) & 0xFF; p[3] =  v         & 0xFF;
    };
    be32(ihdr,     static_cast<uint32_t>(width));
    be32(ihdr + 4, static_cast<uint32_t>(height));
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    write_png_chunk(out, "IHDR", ihdr, 13);

    // 필터 바이트(0=None) + RGBA 스캔라인
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + width * 4));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba + y * static_cast<size_t>(width) * 4;
        raw.insert(raw.end(), row, row + width * 4);
    }

    uLongf clen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(clen);
    if (compress2(compressed.data(), &clen,
                  raw.data(), static_cast<uLong>(raw.size()),
                  Z_BEST_SPEED) != Z_OK) return {};
    compressed.resize(clen);
    write_png_chunk(out, "IDAT", compressed.data(),
                    static_cast<uint32_t>(clen));
    write_png_chunk(out, "IEND", nullptr, 0);
    return out;
}

static std::string base64_encode(const std::vector<uint8_t>& data)
{
    if (data.empty()) return "";
    size_t out_len = 4 * ((data.size() + 2) / 3) + 1;
    std::string result(out_len, '\0');
    int actual = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()),
        data.data(), static_cast<int>(data.size()));
    result.resize(static_cast<size_t>(actual));
    return result;
}

// ── GuacVncClient::Callbacks 구현 ─────────────────────────────────────────

/**
 * GuacVncClient::Callbacks — libvncclient C 콜백을 C++ static 메서드로 구현한다.
 *
 * friend struct이므로 GuacVncClient의 private 멤버에 직접 접근 가능.
 *
 * 각 콜백이 GuacVncClient*를 얻는 방법:
 *   rfbClientGetClientData(client, &kClientDataTag) → void* → static_cast<GuacVncClient*>
 */
struct GuacVncClient::Callbacks {

    /**
     * malloc_framebuffer — 서버 InitMessage 수신 후 프레임버퍼 할당 시 호출.
     *
     * rfbDefaultMallocFrameBuffer가 실제 메모리를 할당한다.
     * 할당 후 Guacamole size 명령어를 전송해 캔버스 크기를 설정한다.
     */
    static rfbBool malloc_framebuffer(rfbClient* rfb)
    {
        // libvncclient 0.9.14에는 rfbDefaultMallocFrameBuffer가 없다.
        // vncviewer.c의 기본 구현을 인라인으로 재현한다:
        //   frameBuffer = realloc(frameBuffer, width * height * bytesPerPixel)
        size_t bytes = static_cast<size_t>(rfb->width) * rfb->height
                       * rfb->format.bitsPerPixel / 8;
        auto* buf = static_cast<uint8_t*>(realloc(rfb->frameBuffer, bytes));
        if (!buf) return FALSE;
        rfb->frameBuffer = buf;

        auto* self = static_cast<GuacVncClient*>(
            rfbClientGetClientData(rfb, &kClientDataTag));
        if (!self) return TRUE;

        GuacInstruction size;
        size.opcode = "size";
        size.args   = { "0",
                        std::to_string(rfb->width),
                        std::to_string(rfb->height) };
        self->callback_(size);
        return TRUE;
    }

    /**
     * got_update — 더티 직사각형 하나를 수신했을 때 호출된다.
     *
     * client->frameBuffer에 해당 영역이 이미 채워져 있다.
     * flush_dirty_region()으로 PNG → Guacamole 명령어 변환을 위임한다.
     */
    static void got_update(rfbClient* rfb, int x, int y, int w, int h)
    {
        auto* self = static_cast<GuacVncClient*>(
            rfbClientGetClientData(rfb, &kClientDataTag));
        if (!self || w <= 0 || h <= 0) return;

        // stride = 전체 화면 너비 × bytes_per_pixel
        int stride = rfb->width * rfb->format.bitsPerPixel / 8;
        self->flush_dirty_region(
            reinterpret_cast<const uint8_t*>(rfb->frameBuffer),
            stride, x, y, w, h);
    }

    /**
     * get_password — VNC 서버가 비밀번호를 요청할 때 호출된다.
     *
     * 반환값은 libvncclient가 free()하므로 strdup으로 복사해 전달한다.
     */
    static char* get_password(rfbClient* rfb)
    {
        auto* self = static_cast<GuacVncClient*>(
            rfbClientGetClientData(rfb, &kClientDataTag));
        if (!self) return strdup("");
        return strdup(self->password_.c_str());
    }
};

// ── 생성자 / 소멸자 ───────────────────────────────────────────────────────

GuacVncClient::GuacVncClient(InstructionCallback callback)
    : callback_(std::move(callback))
    , impl_(std::make_unique<Impl>())
{}

GuacVncClient::~GuacVncClient() {
    disconnect();
}

// ── public API ────────────────────────────────────────────────────────────

void GuacVncClient::connect(const std::string& host, uint16_t port,
                             const std::string& password)
{
    worker_ = std::thread(&GuacVncClient::run_event_loop, this,
                          host, port, password);
}

void GuacVncClient::disconnect() {
    if (impl_) {
        impl_->stop = true;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool GuacVncClient::is_connected() const {
    return connected_.load();
}

// ── flush_dirty_region ────────────────────────────────────────────────────

/**
 * flush_dirty_region — GotFrameBufferUpdate 콜백에서 호출된다.
 *
 * 1. framebuffer에서 더티 영역(x,y,w,h)의 픽셀을 추출한다.
 * 2. BGRX → RGBA 변환 (리틀엔디안 기준: byte0=B, byte1=G, byte2=R, byte3=X)
 * 3. PNG 인코딩 → base64 직렬화
 * 4. Guacamole img → blob(8KB 청크)... → end 명령어 시퀀스를 callback_으로 전달
 */
void GuacVncClient::flush_dirty_region(const uint8_t* framebuffer, int stride,
                                       int x, int y, int w, int h)
{
    // ── 더티 영역 BGRX → RGBA 변환 ──────────────────────────────────────
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (int row = 0; row < h; ++row) {
        const uint8_t* src = framebuffer
                           + (y + row) * stride
                           + x * 4;
        uint8_t* dst = rgba.data() + row * w * 4;
        for (int col = 0; col < w; ++col) {
            dst[0] = src[2];   // R ← src[2]
            dst[1] = src[1];   // G ← src[1]
            dst[2] = src[0];   // B ← src[0]
            dst[3] = 0xFF;     // A = 불투명
            src += 4;
            dst += 4;
        }
    }

    // ── PNG 인코딩 ───────────────────────────────────────────────────────
    std::vector<uint8_t> png = encode_png_rgba(rgba.data(), w, h);
    if (png.empty()) return;

    int stream_id = next_stream_id_++;

    // ── img instruction ──────────────────────────────────────────────────
    // "img", stream_id, compositing_op, layer, mimetype, x, y
    // compositing_op "Over" = 데스크톱 레이어에 그리기
    {
        GuacInstruction img;
        img.opcode = "img";
        img.args   = { std::to_string(stream_id),
                       "Over",            // compositing op
                       "default",         // 레이어
                       "image/png",
                       std::to_string(x),
                       std::to_string(y) };
        callback_(img);
    }

    // ── blob instructions (8KB 청크) ─────────────────────────────────────
    constexpr size_t CHUNK = 8192;
    for (size_t offset = 0; offset < png.size(); offset += CHUNK) {
        size_t chunk_len = std::min(CHUNK, png.size() - offset);
        std::string b64 = base64_encode(
            std::vector<uint8_t>(png.begin() + static_cast<ptrdiff_t>(offset),
                                 png.begin() + static_cast<ptrdiff_t>(offset + chunk_len)));

        GuacInstruction blob;
        blob.opcode = "blob";
        blob.args   = { std::to_string(stream_id), std::move(b64) };
        callback_(blob);
    }

    // ── end instruction ──────────────────────────────────────────────────
    {
        GuacInstruction end;
        end.opcode = "end";
        end.args   = { std::to_string(stream_id) };
        callback_(end);
    }
}

// ── run_event_loop ─────────────────────────────────────────────────────────

void GuacVncClient::run_event_loop(const std::string& host, uint16_t port,
                                   const std::string& password)
{
    password_ = password;

    // ── 1. rfbClient 초기화 ───────────────────────────────────────────────
    // rfbGetClient(bitsPerSample=8, samplesPerPixel=3, bytesPerPixel=4)
    // → 32bpp, depth 24, BGRX 포맷 (리틀엔디안)
    rfbClient* rfb = rfbGetClient(8, 3, 4);
    if (!rfb) return;

    // GuacVncClient* 포인터를 rfb 객체에 주입
    rfbClientSetClientData(rfb, &kClientDataTag, this);

    // ── 2. 콜백 등록 ──────────────────────────────────────────────────────
    rfb->MallocFrameBuffer     = Callbacks::malloc_framebuffer;
    rfb->GotFrameBufferUpdate  = Callbacks::got_update;
    rfb->GetPassword           = Callbacks::get_password;

    // ── 3. 서버 주소 설정 ─────────────────────────────────────────────────
    // rfbInitClient()에 argv를 넘기는 대신 구조체 필드를 직접 설정한다.
    // rfbGetClient()이 strdup("")로 초기화하므로 덮어쓰기 전에 해제 필요.
    free(rfb->serverHost);
    rfb->serverHost = strdup(host.c_str());
    rfb->serverPort = static_cast<int>(port);

    // ── 4. 연결 + 초기 협상 ───────────────────────────────────────────────
    // rfbInitClient(rfb, 0, nullptr): argc/argv를 넘기지 않으면
    // serverHost/serverPort 필드를 그대로 사용한다.
    int argc = 0;
    if (!rfbInitClient(rfb, &argc, nullptr)) {
        // rfbInitClient 실패 시 rfb는 이미 내부에서 해제됨
        impl_->rfb = nullptr;
        return;
    }
    impl_->rfb = rfb;
    connected_.store(true);

    // ── 5. 이벤트 루프 ────────────────────────────────────────────────────
    // WaitForMessage(rfb, usec): 최대 usec 마이크로초 대기 후 준비 여부 반환
    //   > 0: 메시지 있음 → HandleRFBServerMessage 호출
    //   = 0: 타임아웃 (메시지 없음) → 루프 계속
    //   < 0: 오류 → 연결 끊김
    while (!impl_->stop.load()) {
        int ready = WaitForMessage(rfb, 100000);  // 100ms
        if (ready < 0) break;
        if (ready > 0) {
            if (!HandleRFBServerMessage(rfb)) break;
        }
    }

    // ── 6. 정리 ───────────────────────────────────────────────────────────
    connected_.store(false);
    // rfbClientCleanup(0.9.14)은 frameBuffer를 해제하지 않으므로 직접 해제.
    free(rfb->frameBuffer);
    rfb->frameBuffer = nullptr;
    rfbClientCleanup(rfb);
    impl_->rfb = nullptr;
}

} // namespace proxy
