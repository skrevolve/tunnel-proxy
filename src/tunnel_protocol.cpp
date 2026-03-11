#include "core/tunnel_protocol.h"

#include <stdexcept>
#include <cstring>
#include <arpa/inet.h>

namespace proxy {

// ── 직렬화 ──────────────────────────────────────────────────────────────────

std::vector<uint8_t> serialize(const TunnelFrame& frame) {
    std::vector<uint8_t> buf;
    buf.reserve(TUNNEL_HEADER_SIZE + frame.payload.size());

    // [0-3] magic (big-endian)
    uint32_t magic_net = htonl(frame.magic);
    const auto* p = reinterpret_cast<const uint8_t*>(&magic_net);
    buf.insert(buf.end(), p, p + 4);

    // [4] type, [5] flags
    buf.push_back(static_cast<uint8_t>(frame.type));
    buf.push_back(frame.flags);

    // [6-7] reserved (big-endian)
    uint16_t reserved_net = htons(frame.reserved);
    p = reinterpret_cast<const uint8_t*>(&reserved_net);
    buf.insert(buf.end(), p, p + 2);

    // [8-11] session_id (big-endian)
    uint32_t sid_net = htonl(frame.session_id);
    p = reinterpret_cast<const uint8_t*>(&sid_net);
    buf.insert(buf.end(), p, p + 4);

    // [12-15] length (big-endian)
    uint32_t len_net = htonl(frame.length);
    p = reinterpret_cast<const uint8_t*>(&len_net);
    buf.insert(buf.end(), p, p + 4);

    // payload
    buf.insert(buf.end(), frame.payload.begin(), frame.payload.end());

    return buf;
}

// ── 역직렬화 ────────────────────────────────────────────────────────────────

TunnelFrame parse_header(const uint8_t* buf, size_t buf_len) {
    if (buf_len < TUNNEL_HEADER_SIZE) {
        throw std::runtime_error(
            "tunnel: buffer too small for header (" +
            std::to_string(buf_len) + " < " +
            std::to_string(TUNNEL_HEADER_SIZE) + ")");
    }

    TunnelFrame frame;

    // [0-3] magic
    uint32_t magic_net;
    std::memcpy(&magic_net, buf, 4);
    frame.magic = ntohl(magic_net);
    if (frame.magic != TUNNEL_MAGIC) {
        throw std::runtime_error(
            "tunnel: invalid magic bytes (got 0x" +
            [&]() {
                char hex[9];
                snprintf(hex, sizeof(hex), "%08X", frame.magic);
                return std::string(hex);
            }() + ", expected 0x544E4C50)");
    }

    // [4] type, [5] flags
    frame.type  = static_cast<TunnelMsgType>(buf[4]);
    frame.flags = buf[5];

    // [6-7] reserved
    uint16_t reserved_net;
    std::memcpy(&reserved_net, buf + 6, 2);
    frame.reserved = ntohs(reserved_net);

    // [8-11] session_id
    uint32_t sid_net;
    std::memcpy(&sid_net, buf + 8, 4);
    frame.session_id = ntohl(sid_net);

    // [12-15] length
    uint32_t len_net;
    std::memcpy(&len_net, buf + 12, 4);
    frame.length = ntohl(len_net);

    if (frame.length > TUNNEL_MAX_PAYLOAD) {
        throw std::runtime_error(
            "tunnel: payload length exceeds maximum (" +
            std::to_string(frame.length) + " > " +
            std::to_string(TUNNEL_MAX_PAYLOAD) + ")");
    }

    return frame;
}

TunnelFrame parse_frame(const uint8_t* header_buf,
                        const uint8_t* payload_buf, uint32_t payload_len) {
    TunnelFrame frame = parse_header(header_buf, TUNNEL_HEADER_SIZE);

    if (payload_len < frame.length) {
        throw std::runtime_error(
            "tunnel: payload buffer smaller than declared length (" +
            std::to_string(payload_len) + " < " +
            std::to_string(frame.length) + ")");
    }

    if (frame.length > 0) {
        frame.payload.assign(payload_buf, payload_buf + frame.length);
    }

    return frame;
}

// ── 편의 생성 함수 ──────────────────────────────────────────────────────────

TunnelFrame make_hello(const std::string& agent_id) {
    if (agent_id.size() > 255) {
        throw std::invalid_argument(
            "tunnel: agent_id too long (" +
            std::to_string(agent_id.size()) + " > 255)");
    }
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(agent_id.size()));
    payload.insert(payload.end(), agent_id.begin(), agent_id.end());
    return TunnelFrame(TunnelMsgType::HELLO, 0, std::move(payload), TunnelFlags::SYN);
}

TunnelFrame make_hello_ack() {
    return TunnelFrame(TunnelMsgType::HELLO_ACK, 0, {}, TunnelFlags::ACK);
}

TunnelFrame make_open(uint32_t session_id,
                      const std::string& target_ip, uint16_t target_port) {
    std::vector<uint8_t> payload(6);
    in_addr addr{};
    if (inet_pton(AF_INET, target_ip.c_str(), &addr) != 1) {
        throw std::invalid_argument("tunnel: invalid target IP: " + target_ip);
    }
    // addr.s_addr is already in network byte order from inet_pton
    std::memcpy(payload.data(), &addr.s_addr, 4);
    uint16_t port_net = htons(target_port);
    std::memcpy(payload.data() + 4, &port_net, 2);
    return TunnelFrame(TunnelMsgType::OPEN, session_id, std::move(payload), TunnelFlags::SYN);
}

TunnelFrame make_open_ack(uint32_t session_id) {
    return TunnelFrame(TunnelMsgType::OPEN_ACK, session_id, {}, TunnelFlags::ACK);
}

TunnelFrame make_data(uint32_t session_id, std::vector<uint8_t> data) {
    return TunnelFrame(TunnelMsgType::DATA, session_id, std::move(data));
}

TunnelFrame make_close(uint32_t session_id) {
    return TunnelFrame(TunnelMsgType::CLOSE, session_id, {}, TunnelFlags::FIN);
}

TunnelFrame make_heartbeat() {
    return TunnelFrame(TunnelMsgType::HEARTBEAT, 0);
}

TunnelFrame make_heartbeat_ack() {
    return TunnelFrame(TunnelMsgType::HEARTBEAT_ACK, 0, {}, TunnelFlags::ACK);
}

// ── payload 파서 ────────────────────────────────────────────────────────────

std::pair<std::string, uint16_t> parse_open_payload(
    const std::vector<uint8_t>& payload) {
    if (payload.size() < 6) {
        throw std::runtime_error(
            "tunnel: OPEN payload too short (" +
            std::to_string(payload.size()) + " < 6)");
    }
    in_addr addr{};
    std::memcpy(&addr.s_addr, payload.data(), 4);
    char ip_buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr, ip_buf, sizeof(ip_buf))) {
        throw std::runtime_error("tunnel: failed to convert IP from OPEN payload");
    }
    uint16_t port_net;
    std::memcpy(&port_net, payload.data() + 4, 2);
    return {std::string(ip_buf), ntohs(port_net)};
}

std::string parse_hello_payload(const std::vector<uint8_t>& payload) {
    if (payload.empty()) {
        throw std::runtime_error("tunnel: HELLO payload is empty");
    }
    uint8_t len = payload[0];
    if (payload.size() < static_cast<size_t>(1 + len)) {
        throw std::runtime_error(
            "tunnel: HELLO payload truncated (need " +
            std::to_string(1 + len) + ", got " +
            std::to_string(payload.size()) + ")");
    }
    return std::string(reinterpret_cast<const char*>(payload.data() + 1), len);
}

} // namespace proxy
