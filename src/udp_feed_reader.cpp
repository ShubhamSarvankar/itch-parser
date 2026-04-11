#include "udp_feed_reader.h"
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace itch {

// MoldUDP64 constants
static constexpr size_t   MOLD_HEADER_LEN  = 20;   // session(10) + seq(8) + count(2)
static constexpr size_t   MOLD_RECV_BUF    = 2048;
static constexpr uint16_t MOLD_HEARTBEAT   = 0xFFFF;

// ---- socket helpers ----

static int make_udp_socket() {
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        throw std::runtime_error("UDPFeedReader: socket() failed");

    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                 &reuse, sizeof(reuse));
    return sock;
}

static void bind_socket(int sock, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("UDPFeedReader: bind() failed");
}

static void join_multicast(int sock, const std::string& group,
                            const std::string& iface_ip) {
    ip_mreq mreq{};
    if (::inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1)
        throw std::runtime_error("UDPFeedReader: invalid multicast group: " + group);
    if (::inet_pton(AF_INET, iface_ip.c_str(), &mreq.imr_interface) != 1)
        throw std::runtime_error("UDPFeedReader: invalid interface IP: " + iface_ip);
    if (::setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     &mreq, sizeof(mreq)) < 0)
        throw std::runtime_error("UDPFeedReader: IP_ADD_MEMBERSHIP failed");
}

// ---- big-endian read helpers ----

static uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static uint64_t read_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

// ---- constructors ----

UDPFeedReader::UDPFeedReader(const std::string& multicast_group,
                              const std::string& interface_ip,
                              uint16_t port) {
    sock_ = make_udp_socket();
    bind_socket(sock_, port);
    join_multicast(sock_, multicast_group, interface_ip);
}

UDPFeedReader::UDPFeedReader(uint16_t port) {
    // Test constructor — no multicast, just bind on loopback
    sock_ = make_udp_socket();
    bind_socket(sock_, port);
}

UDPFeedReader::~UDPFeedReader() {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

// ---- next_message ----

std::optional<MessageBuffer> UDPFeedReader::next_message() {
    while (true) {
        // Drain pending queue from current packet first
        if (pending_idx_ < pending_.size()) {
            MessageBuffer buf = std::move(pending_[pending_idx_++]);
            ++messages_received_;
            return buf;
        }

        // Queue exhausted — receive next packet
        pending_.clear();
        pending_idx_ = 0;

        if (!refill()) return std::nullopt;  // socket closed
    }
}

// ---- refill ----

bool UDPFeedReader::refill() {
    uint8_t raw[MOLD_RECV_BUF];
    ssize_t n = ::recvfrom(sock_, raw, sizeof(raw), 0, nullptr, nullptr);

    if (n < 0) {
        if (sock_ < 0) return false;  // closed during shutdown
        std::cerr << "[WARN] UDPFeedReader: recvfrom error\n";
        return true;  // keep going
    }

    if (static_cast<size_t>(n) < MOLD_HEADER_LEN) {
        std::cerr << "[WARN] UDPFeedReader: packet too short (" << n << " bytes)\n";
        return true;
    }

    // Parse MoldUDP64 header
    // Offset 0-9:  session (10 bytes, ignored beyond first-packet check)
    // Offset 10-17: sequence number (big-endian uint64_t)
    // Offset 18-19: message count (big-endian uint16_t)
    uint64_t seq       = read_be64(raw + 10);
    uint16_t msg_count = read_be16(raw + 18);

    // Heartbeat — no messages, do not advance expected_seq_
    if (msg_count == MOLD_HEARTBEAT) return true;

    // Empty packet — valid, no messages, no gap accounting
    if (msg_count == 0) return true;

    // Gap detection
    if (seq > expected_seq_) {
        uint64_t gap = seq - expected_seq_;
        gaps_detected_ += gap;
        std::cerr << "[WARN] UDPFeedReader: sequence gap — expected "
                  << expected_seq_ << " got " << seq
                  << " (" << gap << " message(s) lost)\n";
    } else if (seq < expected_seq_) {
        // Duplicate or out-of-order packet — discard without updating state
        return true;
    }

    expected_seq_ = seq + msg_count;

    // Parse message blocks
    size_t offset = MOLD_HEADER_LEN;
    for (uint16_t i = 0; i < msg_count; ++i) {
        if (offset + 2 > static_cast<size_t>(n)) {
            std::cerr << "[WARN] UDPFeedReader: truncated message block header\n";
            break;
        }
        uint16_t msg_len = read_be16(raw + offset);
        offset += 2;

        if (offset + msg_len > static_cast<size_t>(n)) {
            std::cerr << "[WARN] UDPFeedReader: truncated message data\n";
            break;
        }

        MessageBuffer buf(raw + offset, raw + offset + msg_len);
        pending_.push_back(std::move(buf));
        offset += msg_len;
    }

    return true;
}

} // namespace itch