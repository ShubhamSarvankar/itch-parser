#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "itch/message_buffer.h"

namespace itch {

class UDPFeedReader {
public:
    // Production constructor — joins multicast group on interface_ip
    UDPFeedReader(const std::string& multicast_group,
                  const std::string& interface_ip,
                  uint16_t port);

    // Test constructor — binds to 0.0.0.0:port on loopback, no multicast
    explicit UDPFeedReader(uint16_t port);

    ~UDPFeedReader();

    // Returns next ITCH message buffer. Blocks on recvfrom() until a packet
    // arrives. Returns nullopt only on clean shutdown (socket closed).
    std::optional<MessageBuffer> next_message();

    bool     is_open()           const { return sock_ >= 0; }
    uint64_t gaps_detected()     const { return gaps_detected_; }
    uint64_t messages_received() const { return messages_received_; }

private:
    // Receive one UDP packet, parse all MoldUDP64 message blocks into pending_
    bool refill();

    int      sock_{-1};
    uint64_t expected_seq_{1};
    uint64_t gaps_detected_{0};
    uint64_t messages_received_{0};

    // Per-packet message queue — drained by next_message() before next recvfrom()
    std::vector<MessageBuffer> pending_;
    size_t                     pending_idx_{0};
};

} // namespace itch