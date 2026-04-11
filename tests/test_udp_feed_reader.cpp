#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "udp_feed_reader.h"

static constexpr uint16_t TEST_PORT = 19000;

// ---- sender helpers ----

static int make_sender() {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) throw std::runtime_error("make_sender: socket() failed");
    return s;
}

static void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] =  v       & 0xFF;
}

static void write_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = v & 0xFF; v >>= 8; }
}

// Build and send a MoldUDP64 packet containing the given message payloads.
// seq is the sequence number of the first message in the packet.
static void send_mold_packet(int sock, uint16_t port, uint64_t seq,
                              const std::vector<itch::MessageBuffer>& messages) {
    std::vector<uint8_t> pkt;
    pkt.resize(20);  // header placeholder

    // Session (10 bytes) — all spaces
    for (int i = 0; i < 10; ++i) pkt[i] = ' ';
    write_be64(pkt.data() + 10, seq);
    write_be16(pkt.data() + 18, static_cast<uint16_t>(messages.size()));

    for (const auto& msg : messages) {
        uint8_t len_bytes[2];
        write_be16(len_bytes, static_cast<uint16_t>(msg.size()));
        pkt.push_back(len_bytes[0]);
        pkt.push_back(len_bytes[1]);
        pkt.insert(pkt.end(), msg.begin(), msg.end());
    }

    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::sendto(sock, pkt.data(), pkt.size(), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

// Send a heartbeat packet (msg_count = 0xFFFF)
static void send_heartbeat(int sock, uint16_t port, uint64_t seq) {
    uint8_t pkt[20];
    for (int i = 0; i < 10; ++i) pkt[i] = ' ';
    write_be64(pkt + 10, seq);
    write_be16(pkt + 18, 0xFFFF);

    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::sendto(sock, pkt, sizeof(pkt), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

// Send an empty packet (msg_count = 0)
static void send_empty_packet(int sock, uint16_t port, uint64_t seq) {
    uint8_t pkt[20];
    for (int i = 0; i < 10; ++i) pkt[i] = ' ';
    write_be64(pkt + 10, seq);
    write_be16(pkt + 18, 0);

    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(port);
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::sendto(sock, pkt, sizeof(pkt), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

// Make a simple payload of known bytes for integrity checks
static itch::MessageBuffer make_payload(uint8_t first_byte, size_t len = 16) {
    itch::MessageBuffer buf(len, 0xAB);
    buf[0] = first_byte;
    return buf;
}

// ---- fixture ----
// Each test uses a fresh reader on TEST_PORT and a fresh sender socket.
// Tests send packets then read from the reader synchronously on loopback —
// no timing issues since loopback delivery is synchronous.

class UDPFeedReaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        sender_ = make_sender();
        reader_ = std::make_unique<itch::UDPFeedReader>(TEST_PORT);
    }

    void TearDown() override {
        ::close(sender_);
        // reader_ destructor closes the socket
    }

    int send_sock() const { return sender_; }

    // Send packets then read exactly n messages from the reader.
    // Returns the collected buffers.
    std::vector<itch::MessageBuffer> read_n(int n) {
        std::vector<itch::MessageBuffer> result;
        for (int i = 0; i < n; ++i) {
            auto msg = reader_->next_message();
            EXPECT_TRUE(msg.has_value());
            if (msg) result.push_back(std::move(*msg));
        }
        return result;
    }

    int                                  sender_;
    std::unique_ptr<itch::UDPFeedReader> reader_;
};

// ---- tests ----

TEST_F(UDPFeedReaderTest, SingleMessageInPacket) {
    auto payload = make_payload('A');
    send_mold_packet(sender_, TEST_PORT, 1, {payload});

    auto msgs = read_n(1);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0], payload);
    EXPECT_EQ(reader_->messages_received(), 1u);
}

TEST_F(UDPFeedReaderTest, MultipleMessagesInOnePacket) {
    std::vector<itch::MessageBuffer> payloads = {
        make_payload('A', 36),
        make_payload('D', 19),
        make_payload('E', 31),
    };
    send_mold_packet(sender_, TEST_PORT, 1, payloads);

    auto msgs = read_n(3);
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0], payloads[0]);
    EXPECT_EQ(msgs[1], payloads[1]);
    EXPECT_EQ(msgs[2], payloads[2]);
    EXPECT_EQ(reader_->messages_received(), 3u);
}

TEST_F(UDPFeedReaderTest, MultiplePacketsSequential_NoGap) {
    send_mold_packet(sender_, TEST_PORT, 1, {make_payload('A')});
    send_mold_packet(sender_, TEST_PORT, 2, {make_payload('D')});
    send_mold_packet(sender_, TEST_PORT, 3, {make_payload('E')});

    auto msgs = read_n(3);
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(reader_->gaps_detected(), 0u);
    EXPECT_EQ(reader_->messages_received(), 3u);
}

TEST_F(UDPFeedReaderTest, SequenceGap_DetectedAndCounted) {
    // seq=1 (1 message), then seq=5 (1 message) — gap of 3
    send_mold_packet(sender_, TEST_PORT, 1, {make_payload('A')});
    send_mold_packet(sender_, TEST_PORT, 5, {make_payload('D')});

    auto msgs = read_n(2);
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(reader_->gaps_detected(), 3u);  // seqs 2, 3, 4 missing
}

TEST_F(UDPFeedReaderTest, HeartbeatPacket_ProducesNoMessage) {
    // Heartbeat followed by a real packet — only the real message is returned
    send_heartbeat(sender_, TEST_PORT, 1);
    send_mold_packet(sender_, TEST_PORT, 1, {make_payload('R')});

    auto msgs = read_n(1);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0][0], 'R');
    EXPECT_EQ(reader_->messages_received(), 1u);
}

TEST_F(UDPFeedReaderTest, HeartbeatPacket_DoesNotAffectGapCounting) {
    // Heartbeat between two sequential real packets — no gap
    send_mold_packet(sender_, TEST_PORT, 1, {make_payload('A')});
    send_heartbeat(sender_, TEST_PORT, 2);   // heartbeat doesn't advance seq
    send_mold_packet(sender_, TEST_PORT, 2, {make_payload('D')});

    auto msgs = read_n(2);
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(reader_->gaps_detected(), 0u);
}

TEST_F(UDPFeedReaderTest, EmptyPacket_ProducesNoMessage_NoGap) {
    send_empty_packet(sender_, TEST_PORT, 1);
    send_mold_packet(sender_, TEST_PORT, 1, {make_payload('A')});

    auto msgs = read_n(1);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(reader_->gaps_detected(), 0u);
    EXPECT_EQ(reader_->messages_received(), 1u);
}

TEST_F(UDPFeedReaderTest, MessagePayloadIntegrity) {
    // 36-byte payload with known content
    itch::MessageBuffer payload(36);
    for (size_t i = 0; i < 36; ++i)
        payload[i] = static_cast<uint8_t>(i);

    send_mold_packet(sender_, TEST_PORT, 1, {payload});

    auto msgs = read_n(1);
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].size(), 36u);
    EXPECT_EQ(msgs[0], payload);
}

TEST_F(UDPFeedReaderTest, MultipleGaps_Accumulate) {
    // seq 1, then 3 (gap=1), then 7 (gap=3) — total 4
    send_mold_packet(sender_, TEST_PORT, 1, {make_payload('A')});
    send_mold_packet(sender_, TEST_PORT, 3, {make_payload('D')});
    send_mold_packet(sender_, TEST_PORT, 7, {make_payload('E')});

    read_n(3);
    EXPECT_EQ(reader_->gaps_detected(), 4u);
}

TEST_F(UDPFeedReaderTest, DuplicatePacket_Discarded) {
    // seq=1 twice — second is duplicate, should not produce a message
    // or affect gap counting
    send_mold_packet(sender_, TEST_PORT, 1, {make_payload('A')});
    send_mold_packet(sender_, TEST_PORT, 1, {make_payload('D')});  // duplicate
    send_mold_packet(sender_, TEST_PORT, 2, {make_payload('E')});  // continues normally

    auto msgs = read_n(2);
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0][0], 'A');
    EXPECT_EQ(msgs[1][0], 'E');  // 'D' from duplicate was discarded
    EXPECT_EQ(reader_->gaps_detected(), 0u);
    EXPECT_EQ(reader_->messages_received(), 2u);
}