/**
 * @file guac_rdp.cpp
 * @brief Phase 8-B — FreeRDP 기반 RDP 클라이언트 구현 + Guacamole 화면 스트리밍
 *
 * ── 구현 개요 ─────────────────────────────────────────────────────────────
 *
 *   FreeRDP 3.x를 사용해 RDP 서버에 연결한다.
 *   GDI 소프트웨어 레이어로 화면을 BGRA32 픽셀 버퍼(gdi->primary_buffer)에 렌더링.
 *   EndPaint 콜백에서 더티 영역을 PNG(zlib DEFLATE)로 인코딩하고
 *   OpenSSL base64로 직렬화해 Guacamole img/blob/end 명령어로 내보낸다.
 *
 * ── FreeRDP 컨텍스트 확장 패턴 ──────────────────────────────────────────
 *
 *   GuacRdpClientCtx::base를 첫 멤버로 두면 rdpContext* ↔ GuacRdpClientCtx*
 *   사이를 안전하게 캐스팅할 수 있다 (C의 구조체 레이아웃 규칙).
 *
 * ── Callbacks 중첩 구조체 (friend) ──────────────────────────────────────
 *
 *   GuacRdpClient::Callbacks는 GuacRdpClient의 friend이므로 private 멤버에
 *   직접 접근 가능하다. FreeRDP C 콜백을 C++ static 멤버 함수로 구현하면
 *   C 함수 포인터 호환성을 유지하면서 클래스 private 멤버를 사용할 수 있다.
 *
 * ── PNG 인코딩 (inline, zlib만 사용) ─────────────────────────────────────
 *
 *   외부 PNG 라이브러리 없이 zlib compress2() + crc32()로 PNG를 직접 생성한다.
 *   PNG 포맷: 8B 서명 + IHDR 청크 + IDAT 청크 + IEND 청크.
 *   compress2()는 zlib-wrapped DEFLATE를 출력하며 PNG IDAT에 직접 사용 가능하다.
 */

// ── FreeRDP 헤더 (guac_rdp.h에는 노출되지 않음) ─────────────────────────────
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <freerdp/gdi/gdi.h>
#include <winpr/wtypes.h>
#include <winpr/synch.h>
#include <winpr/handle.h>

// ── 표준 / 프로젝트 헤더 ─────────────────────────────────────────────────
#include "core/guac_rdp.h"
#include "core/guac_parser.h"

#include <openssl/evp.h>        // EVP_EncodeBlock (base64)
#include <zlib.h>               // compress2, crc32, compressBound

#include <stdexcept>
#include <cstring>
#include <arpa/inet.h>          // htonl

namespace proxy {

// ── FreeRDP 컨텍스트 확장 ────────────────────────────────────────────────

/**
 * GuacRdpClientCtx — rdpContext를 확장해 C++ 객체 포인터를 주입한다.
 *
 * 왜 base가 첫 번째 멤버여야 하는가:
 *   FreeRDP는 내부적으로 rdpContext* 포인터를 사용한다.
 *   rdpContext* → GuacRdpClientCtx* 캐스팅이 안전하려면
 *   base 필드가 구조체의 맨 앞에 위치해 주소가 동일해야 한다 (C 레이아웃 규칙).
 */
struct GuacRdpClientCtx {
    rdpContext     base;    // ← 반드시 첫 번째 멤버
    GuacRdpClient* client;  // GuacRdpClient* 역참조용
};

// ── GuacRdpClient::Impl ──────────────────────────────────────────────────

struct GuacRdpClient::Impl {
    freerdp* instance = nullptr;
};

// ── PNG 인코딩 헬퍼 ──────────────────────────────────────────────────────

/**
 * write_png_chunk — PNG 청크 하나를 out 버퍼에 추가한다.
 *
 * PNG 청크 포맷: [4B length BE][4B type][N B data][4B CRC32(type+data)]
 * CRC32 계산에 zlib의 crc32()를 사용한다.
 */
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

/**
 * encode_png_rgba — RGBA32 픽셀 버퍼를 PNG 바이트 시퀀스로 인코딩한다.
 *
 * 픽셀 포맷: rgba[0]=R, rgba[1]=G, rgba[2]=B, rgba[3]=A (colortype 6, bitdepth 8)
 * stride = width * 4 (패딩 없음, 이미 추출된 더티 영역 버퍼)
 *
 * PNG IDAT 포맷: 각 스캔라인에 filter_byte(0=None) 추가 후
 *               compress2()로 zlib-wrapped DEFLATE 압축 → IDAT 청크
 *
 * @return PNG 바이트 벡터. 인코딩 실패 시 빈 벡터.
 */
static std::vector<uint8_t> encode_png_rgba(
        const uint8_t* rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0) return {};

    static const uint8_t PNG_SIG[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };

    std::vector<uint8_t> out;
    out.insert(out.end(), PNG_SIG, PNG_SIG + 8);

    // IHDR: width(4BE), height(4BE), bitdepth(1), colortype(1=RGBA=6),
    //       compression(1), filter(1), interlace(1)
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
        const uint8_t* row = rgba + y * width * 4;
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

/**
 * base64_encode — 바이트 시퀀스를 base64 문자열로 인코딩한다.
 * Guacamole blob 명령어의 페이로드 형식이다.
 * OpenSSL EVP_EncodeBlock을 사용해 추가 의존성 없이 구현한다.
 */
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

// ── GuacRdpClient::Callbacks 구현 ────────────────────────────────────────

/**
 * GuacRdpClient::Callbacks — FreeRDP C 콜백을 C++ static 메서드로 구현한다.
 *
 * friend struct이므로 GuacRdpClient의 private 멤버에 직접 접근 가능.
 * C 함수 포인터 호환성: static 멤버 함수는 C calling convention과 동일하다.
 *
 * 각 콜백이 GuacRdpClient*를 얻는 방법:
 *   rdpContext* → reinterpret_cast<GuacRdpClientCtx*> → .client
 */
struct GuacRdpClient::Callbacks {

    // ContextNew: freerdp_context_new() 내부에서 호출됨
    // client 포인터는 이 시점에 아직 설정되지 않음 (null 초기화)
    static BOOL context_new(freerdp* /*instance*/, rdpContext* ctx)
    {
        reinterpret_cast<GuacRdpClientCtx*>(ctx)->client = nullptr;
        return TRUE;
    }

    static void context_free(freerdp* /*instance*/, rdpContext* /*ctx*/) {}

    // PreConnect: freerdp_connect() 직전에 호출됨
    // 설정은 run_event_loop()에서 미리 완료되어 있으므로 여기서는 추가 작업 없음
    static BOOL pre_connect(freerdp* /*instance*/)
    {
        return TRUE;
    }

    /**
     * PostConnect: RDP 핸드셰이크 완료 후 호출됨.
     *
     * 1. gdi_init()으로 소프트웨어 GDI 레이어 초기화
     *    → gdi->primary_buffer가 BGRA32 framebuffer로 할당됨
     *    → BitmapUpdate 등 디코딩 콜백이 자동으로 설정됨
     * 2. EndPaint 콜백을 우리 핸들러로 교체
     *    → GDI가 한 프레임을 다 그리고 나면 end_paint() 호출
     * 3. connected_ 플래그 설정 + size 명령어 전송
     *
     * 왜 PIXEL_FORMAT_BGRA32인가:
     *   FreeRDP 기본 포맷. 별도 포맷 변환 없이 GDI가 직접 기록.
     *   우리 쪽에서 BGRA→RGBA 변환을 명시적으로 수행 (PNG는 RGBA 기대).
     */
    static BOOL post_connect(freerdp* instance)
    {
        if (!gdi_init(instance, PIXEL_FORMAT_BGRA32))
            return FALSE;

        // EndPaint 교체 — GDI 초기화 이후에만 유효
        instance->context->update->EndPaint = end_paint;

        auto* gc = reinterpret_cast<GuacRdpClientCtx*>(instance->context);
        GuacRdpClient* c = gc->client;
        if (!c) return TRUE;

        c->connected_.store(true);

        // 브라우저에 캔버스 크기 설정 (첫 번째 Guacamole 명령어)
        rdpGdi* gdi = instance->context->gdi;
        c->callback_(GuacInstruction{
            "size", {"0",
                     std::to_string(gdi->width),
                     std::to_string(gdi->height)}
        });
        return TRUE;
    }

    static void post_disconnect(freerdp* instance)
    {
        gdi_free(instance);
        auto* gc = reinterpret_cast<GuacRdpClientCtx*>(instance->context);
        if (gc->client) gc->client->connected_.store(false);
    }

    /**
     * end_paint: GDI가 한 프레임을 다 그린 뒤 호출된다.
     *
     * 더티 영역(gdi->primary->hdc->hwnd->invalid)을 읽고,
     * flush_dirty_region()을 호출해 PNG 인코딩 + Guacamole 명령어 전송.
     * 처리 후 dirty 플래그(invalid->null = TRUE)를 리셋해 중복 처리 방지.
     *
     * FreeRDP 3.x GDI dirty region 구조:
     *   gdi->primary->hdc->hwnd->invalid (HGDI_RGN = GDI_RGN*)
     *   invalid->null  : TRUE이면 더티 영역 없음
     *   invalid->x/y   : 더티 영역 좌상단 (INT32)
     *   invalid->w/h   : 더티 영역 크기 (UINT32)
     *
     * 기존 `gdi_end_paint`가 없는 이유:
     *   FreeRDP 3.x에서 `gdi_end_paint`는 공개 API가 아니다.
     *   GDI 레이어는 BitmapUpdate 콜백을 통해 framebuffer를 직접 갱신하며,
     *   EndPaint에서는 dirty 플래그를 리셋하면 충분하다.
     */
    static BOOL end_paint(rdpContext* ctx)
    {
        rdpGdi* gdi = ctx->gdi;
        if (!gdi || !gdi->primary || !gdi->primary->hdc ||
            !gdi->primary->hdc->hwnd) {
            return TRUE;
        }

        HGDI_RGN invalid = gdi->primary->hdc->hwnd->invalid;
        if (!invalid || invalid->null)
            return TRUE; // 더티 영역 없음

        INT32  x = invalid->x;
        INT32  y = invalid->y;
        UINT32 w = invalid->w;
        UINT32 h = invalid->h;

        // dirty 플래그 리셋 (처리 전에 리셋해 재진입 방지)
        invalid->null = TRUE;

        auto* gc = reinterpret_cast<GuacRdpClientCtx*>(ctx);
        GuacRdpClient* c = gc->client;
        if (c && w > 0 && h > 0) {
            c->flush_dirty_region(
                gdi->primary_buffer,
                gdi->stride,
                static_cast<int>(x),
                static_cast<int>(y),
                static_cast<int>(w),
                static_cast<int>(h)
            );
        }
        return TRUE;
    }
};

// ── GuacRdpClient 구현 ─────────────────────────────────────────────────

GuacRdpClient::GuacRdpClient(InstructionCallback callback)
    : callback_(std::move(callback))
    , impl_(std::make_unique<Impl>())
{}

GuacRdpClient::~GuacRdpClient()
{
    if (connected_.load()) disconnect();
}

void GuacRdpClient::connect(const std::string& host, uint16_t port,
                             const std::string& username,
                             const std::string& password,
                             uint16_t width, uint16_t height)
{
    if (connected_.load())
        throw std::runtime_error("GuacRdpClient: already connected");

    worker_ = std::thread([this, host, port, username, password, width, height]() {
        run_event_loop(host, port, username, password, width, height);
    });
}

void GuacRdpClient::disconnect()
{
    if (impl_->instance) {
        freerdp_disconnect(impl_->instance);
    }
    if (worker_.joinable()) worker_.join();
    connected_.store(false);
}

bool GuacRdpClient::is_connected() const
{
    return connected_.load();
}

// ── FreeRDP 이벤트 루프 ───────────────────────────────────────────────────

/**
 * run_event_loop — worker_ 스레드에서 실행된다.
 *
 * 1. freerdp_new() + 콜백 등록 + ContextSize 확장
 * 2. freerdp_context_new() → GuacRdpClientCtx::client 설정
 * 3. 설정(hostname/port/credentials/GDI 옵션) 적용
 * 4. freerdp_connect() → 내부에서 pre_connect → post_connect 순으로 호출
 * 5. WaitForMultipleObjects + freerdp_check_event_handles 루프
 *    - 100ms 타임아웃으로 종료 신호에 즉시 응답
 * 6. freerdp_disconnect() + context_free() + free()
 *
 * 왜 WaitForMultipleObjects(timeout=100ms)인가:
 *   FreeRDP는 WinPR HANDLE 기반 이벤트를 사용한다.
 *   INFINITE로 대기하면 종료 신호(disconnect())를 받지 못할 수 있다.
 *   100ms 타임아웃으로 freerdp_shall_disconnect_context()를 주기적으로 확인한다.
 */
void GuacRdpClient::run_event_loop(const std::string& host, uint16_t port,
                                    const std::string& username,
                                    const std::string& password,
                                    uint16_t width, uint16_t height)
{
    // 1. freerdp 인스턴스 생성
    freerdp* instance = freerdp_new();
    if (!instance) return;
    impl_->instance = instance;

    // 컨텍스트 크기를 GuacRdpClientCtx로 확장
    instance->ContextSize    = sizeof(GuacRdpClientCtx);
    instance->ContextNew     = Callbacks::context_new;
    instance->ContextFree    = Callbacks::context_free;
    instance->PreConnect     = Callbacks::pre_connect;
    instance->PostConnect    = Callbacks::post_connect;
    instance->PostDisconnect = Callbacks::post_disconnect;

    if (!freerdp_context_new(instance)) {
        freerdp_free(instance);
        impl_->instance = nullptr;
        return;
    }

    // 2. GuacRdpClient* 역참조 등록 (context_new 이후에 설정)
    reinterpret_cast<GuacRdpClientCtx*>(instance->context)->client = this;

    // 3. RDP 연결 설정
    rdpSettings* settings = instance->context->settings;

    freerdp_settings_set_string(settings, FreeRDP_ServerHostname, host.c_str());
    freerdp_settings_set_uint32(settings, FreeRDP_ServerPort,     port);
    freerdp_settings_set_string(settings, FreeRDP_Username,       username.c_str());
    freerdp_settings_set_string(settings, FreeRDP_Password,       password.c_str());
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,   width);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,  height);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth,     32);

    // 소프트웨어 GDI — 하드웨어 GPU 없이 서버 측 렌더링
    freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE);

    // 개발/테스트 편의: 자체 서명 인증서 허용
    // 프로덕션 환경에서는 FALSE로 설정하고 인증서를 검증해야 함
    freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);

    // 비트맵 캐시 비활성화 — 항상 전체 비트맵 업데이트 수신
    freerdp_settings_set_bool(settings, FreeRDP_BitmapCacheEnabled, FALSE);

    // 4. RDP 연결 (블로킹 — 내부에서 pre/post_connect 콜백 호출)
    if (!freerdp_connect(instance)) {
        freerdp_context_free(instance);
        freerdp_free(instance);
        impl_->instance = nullptr;
        return;
    }

    // 5. 이벤트 루프
    HANDLE handles[64] = {};
    while (!freerdp_shall_disconnect_context(instance->context)) {
        DWORD count = freerdp_get_event_handles(
            instance->context, handles,
            static_cast<DWORD>(sizeof(handles) / sizeof(handles[0])));
        if (count == 0) break;

        DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
        if (status == WAIT_FAILED) break;

        if (!freerdp_check_event_handles(instance->context)) break;
    }

    // 6. 정리
    freerdp_disconnect(instance);
    freerdp_context_free(instance);
    freerdp_free(instance);
    impl_->instance = nullptr;
    connected_.store(false);
}

// ── 더티 영역 PNG 인코딩 + Guacamole 명령어 생성 ──────────────────────────

/**
 * flush_dirty_region — 더티 영역을 PNG로 인코딩해 Guacamole 명령어로 내보낸다.
 *
 * 처리 흐름:
 *   1. BGRA32 framebuffer에서 더티 영역 픽셀 추출
 *   2. BGRA → RGBA 채널 스왑 (PNG는 RGBA 순서)
 *   3. encode_png_rgba()로 PNG 인코딩 (zlib compress2)
 *   4. base64_encode()로 PNG 바이트를 ASCII로 변환
 *   5. img / blob... / end GuacInstruction 생성 + callback_ 호출
 *
 * Guacamole img/blob/end 프로토콜:
 *   img : "img", stream_id, compositing_op(14=src-over), layer(0),
 *               mimetype("image/png"), x, y
 *   blob: "blob", stream_id, <base64-PNG-8KB청크>
 *   end : "end",  stream_id
 *
 * 왜 8KB 청크인가:
 *   WebSocket 프레임 크기 제한 대응.
 *   Guacamole 공식 구현의 기본 청크 크기와 동일.
 *
 * stride vs width*4:
 *   FreeRDP GDI primary_buffer의 스캔라인 바이트 수는 gdi->stride로 주어진다.
 *   width * 4와 같을 수도 있지만 정렬(alignment) 패딩이 있을 수 있으므로
 *   stride를 사용해야 픽셀을 정확히 추출할 수 있다.
 */
void GuacRdpClient::flush_dirty_region(const uint8_t* framebuffer,
                                        uint32_t stride,
                                        int dirty_x, int dirty_y,
                                        int dirty_w, int dirty_h)
{
    // 1. 더티 영역 픽셀 추출 + BGRA → RGBA 변환
    std::vector<uint8_t> rgba(static_cast<size_t>(dirty_w * dirty_h * 4));

    for (int row = 0; row < dirty_h; ++row) {
        const uint8_t* src = framebuffer
                             + (static_cast<size_t>(dirty_y + row)) * stride
                             + static_cast<size_t>(dirty_x) * 4;
        uint8_t* dst = rgba.data() + row * dirty_w * 4;

        for (int col = 0; col < dirty_w; ++col) {
            // BGRA (FreeRDP GDI) → RGBA (PNG)
            dst[col * 4 + 0] = src[col * 4 + 2]; // R ← B
            dst[col * 4 + 1] = src[col * 4 + 1]; // G
            dst[col * 4 + 2] = src[col * 4 + 0]; // B ← R
            dst[col * 4 + 3] = src[col * 4 + 3]; // A
        }
    }

    // 2. PNG 인코딩
    std::vector<uint8_t> png = encode_png_rgba(rgba.data(), dirty_w, dirty_h);
    if (png.empty()) return;

    // 3. base64 인코딩
    std::string b64 = base64_encode(png);

    // 4. Guacamole 명령어 생성
    std::string sid = std::to_string(next_stream_id_++);
    std::string sx  = std::to_string(dirty_x);
    std::string sy  = std::to_string(dirty_y);

    // img: 새 이미지 스트림 시작 (compositing_op=14=src-over, layer=0)
    callback_(GuacInstruction{"img", {sid, "14", "0", "image/png", sx, sy}});

    // blob: PNG 데이터를 8KB 청크로 분할 전송
    const size_t CHUNK = 8192;
    for (size_t i = 0; i < b64.size(); i += CHUNK) {
        callback_(GuacInstruction{"blob", {sid, b64.substr(i, CHUNK)}});
    }

    // end: 이미지 스트림 종료 → 브라우저에 렌더링 지시
    callback_(GuacInstruction{"end", {sid}});
}

} // namespace proxy
