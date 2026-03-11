#include "core/tunnel_protocol.h"

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <cstring>

using namespace proxy;

// ─── 시나리오 1: 프레임 직렬화/파싱 왕복 ─────────────────────────────────────
TEST(TunnelProtocol, SerializeParseRoundtrip) {
    std::vector<uint8_t> payload = {'h', 'e', 'l', 'l', 'o'};
    TunnelFrame original(TunnelMsgType::DATA, 42, payload, TunnelFlags::ACK);

    auto wire = serialize(original);
    ASSERT_EQ(wire.size(), TUNNEL_HEADER_SIZE + payload.size());

    TunnelFrame parsed = parse_frame(
        wire.data(),
        wire.data() + TUNNEL_HEADER_SIZE,
        static_cast<uint32_t>(payload.size()));

    EXPECT_EQ(parsed.magic,      TUNNEL_MAGIC);
    EXPECT_EQ(parsed.type,       TunnelMsgType::DATA);
    EXPECT_EQ(parsed.flags,      TunnelFlags::ACK);
    EXPECT_EQ(parsed.session_id, 42u);
    EXPECT_EQ(parsed.length,     static_cast<uint32_t>(payload.size()));
    EXPECT_EQ(parsed.payload,    payload);
}

// ─── 시나리오 2: magic 불일치 → parse_header() 예외 ──────────────────────────
TEST(TunnelProtocol, InvalidMagic_Throws) {
    TunnelFrame f(TunnelMsgType::HEARTBEAT, 0);
    auto wire = serialize(f);
    // magic 첫 바이트 변조
    wire[0] ^= 0xFF;

    EXPECT_THROW(parse_header(wire.data(), wire.size()), std::runtime_error);
}

// ─── 시나리오 3: length 초과 → parse_header() 예외 ───────────────────────────
TEST(TunnelProtocol, LengthExceedsMax_Throws) {
    TunnelFrame f(TunnelMsgType::DATA, 1);
    auto wire = serialize(f);

    // length 필드(바이트 12-15)를 TUNNEL_MAX_PAYLOAD + 1로 덮어씀
    uint32_t bad_len = htonl(TUNNEL_MAX_PAYLOAD + 1);
    std::memcpy(wire.data() + 12, &bad_len, 4);

    EXPECT_THROW(parse_header(wire.data(), wire.size()), std::runtime_error);
}

// ─── 시나리오 4: make_hello / parse_hello_payload 왕복 ───────────────────────
TEST(TunnelProtocol, HelloPayloadRoundtrip) {
    const std::string agent_id = "my-agent-001";
    TunnelFrame frame = make_hello(agent_id);

    EXPECT_EQ(frame.type,       TunnelMsgType::HELLO);
    EXPECT_EQ(frame.session_id, 0u);  // 컨트롤 채널

    std::string parsed_id = parse_hello_payload(frame.payload);
    EXPECT_EQ(parsed_id, agent_id);
}

// ─── 시나리오 5: make_open / parse_open_payload 왕복 (네트워크 바이트 오더) ──
TEST(TunnelProtocol, OpenPayloadRoundtrip) {
    const std::string target_ip   = "192.168.1.10";
    const uint16_t    target_port = 8080;
    TunnelFrame frame = make_open(7, target_ip, target_port);

    EXPECT_EQ(frame.type,       TunnelMsgType::OPEN);
    EXPECT_EQ(frame.session_id, 7u);
    EXPECT_EQ(frame.payload.size(), 6u);  // 4바이트 IP + 2바이트 포트

    auto [ip, port] = parse_open_payload(frame.payload);
    EXPECT_EQ(ip,   target_ip);
    EXPECT_EQ(port, target_port);
}

// ─── 시나리오 6: 각 메시지 타입 팩토리 — type 필드 확인 ──────────────────────
TEST(TunnelProtocol, FactoryFunctions_CorrectType) {
    EXPECT_EQ(make_hello("x").type,       TunnelMsgType::HELLO);
    EXPECT_EQ(make_hello_ack().type,      TunnelMsgType::HELLO_ACK);
    EXPECT_EQ(make_open(1,"1.2.3.4",80).type, TunnelMsgType::OPEN);
    EXPECT_EQ(make_open_ack(1).type,      TunnelMsgType::OPEN_ACK);
    EXPECT_EQ(make_data(1,{}).type,       TunnelMsgType::DATA);
    EXPECT_EQ(make_close(1).type,         TunnelMsgType::CLOSE);
    EXPECT_EQ(make_heartbeat().type,      TunnelMsgType::HEARTBEAT);
    EXPECT_EQ(make_heartbeat_ack().type,  TunnelMsgType::HEARTBEAT_ACK);
}

// ─── 시나리오 추가: parse_open_payload payload 크기 부족 → 예외 ──────────────
TEST(TunnelProtocol, OpenPayloadTooShort_Throws) {
    std::vector<uint8_t> short_payload = {0x01, 0x02};  // 6바이트 미만
    EXPECT_THROW(parse_open_payload(short_payload), std::runtime_error);
}

// ─── 시나리오 추가: session_id 0은 컨트롤 채널, DATA는 1 이상 ─────────────────
TEST(TunnelProtocol, ControlChannelSessionId) {
    EXPECT_EQ(make_heartbeat().session_id,     0u);
    EXPECT_EQ(make_heartbeat_ack().session_id, 0u);
    EXPECT_EQ(make_hello("a").session_id,      0u);
    EXPECT_EQ(make_hello_ack().session_id,     0u);

    EXPECT_EQ(make_close(5).session_id,        5u);
    EXPECT_EQ(make_open_ack(3).session_id,     3u);
}
