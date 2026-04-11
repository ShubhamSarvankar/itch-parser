#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <atomic>
#include "feed_reader.h"
#include "udp_feed_reader.h"
#include "parser.h"
#include "book_engine.h"
#include "snapshot_publisher.h"
#include "rest_server.h"
#include "concurrentqueue.h"
#include <unistd.h>

static std::string env(const char* key, const char* fallback) {
    const char* val = std::getenv(key);
    return val ? val : fallback;
}

int main() {
    std::string mode             = env("ITCH_MODE",           "file");
    std::string itch_file        = env("ITCH_FILE",           "data/sample.bin");
    std::string mcast_group      = env("MCAST_GROUP",         "233.54.12.111");
    std::string mcast_iface      = env("MCAST_IFACE",         "eth0");
    uint16_t    mcast_port       = static_cast<uint16_t>(
                                     std::stoi(env("MCAST_PORT", "26477")));
    uint16_t    rest_port        = static_cast<uint16_t>(
                                     std::stoi(env("REST_PORT", "8080")));
    uint64_t    snapshot_interval= std::stoull(env("SNAPSHOT_INTERVAL", "1000"));

    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    engine.set_snapshot_interval(snapshot_interval);

    itch::RestServer rest(publisher, rest_port);
    rest.start();

    std::cout << "itch-parser starting\n";
    std::cout << "mode:    " << mode << "\n";
    std::cout << "REST:    http://localhost:" << rest_port << "\n";

    if (mode == "live") {
        // Two-thread pipeline: receive thread → queue → engine thread
        std::cout << "multicast: " << mcast_group << ":" << mcast_port
                  << " on " << mcast_iface << "\n";

        moodycamel::ConcurrentQueue<itch::MessageBuffer> queue;
        std::atomic<bool> recv_done{false};

        // Receive thread: UDPFeedReader → enqueue
        std::jthread recv_thread([&](std::stop_token stop) {
            try {
                itch::UDPFeedReader reader(mcast_group, mcast_iface, mcast_port);
                std::cout << "[INFO] UDP receive thread started\n";
                while (!stop.stop_requested()) {
                    auto buf = reader.next_message();
                    if (!buf) break;
                    queue.enqueue(std::move(*buf));
                }
                std::cerr << "[INFO] gaps detected: "
                          << reader.gaps_detected() << "\n";
            } catch (const std::exception& e) {
                std::cerr << "[ERROR] receive thread: " << e.what() << "\n";
            }
            recv_done = true;
        });

        // Engine thread: dequeue → parse → apply (runs on main thread)
        itch::MessageParser  parser;
        itch::MessageBuffer  buf;
        std::cout << "[INFO] engine thread running — waiting for packets\n";
        while (!recv_done || queue.try_dequeue(buf)) {
            if (queue.try_dequeue(buf)) {
                try {
                    auto msg = parser.parse(buf);
                    if (msg) engine.apply(*msg);
                } catch (const std::exception& e) {
                    std::cerr << "[WARN] parse error: " << e.what() << "\n";
                }
            } else {
                std::this_thread::yield();
            }
        }

    } else {
        // File mode — single-threaded pipeline, unchanged from Phase 1
        std::cout << "reading: " << itch_file << "\n";
        try {
            itch::FileFeedReader reader(itch_file);
            itch::MessageParser  parser;

            while (auto buf = reader.next_message()) {
                try {
                    auto msg = parser.parse(*buf);
                    if (msg) engine.apply(*msg);
                } catch (const std::exception& e) {
                    std::cerr << "[WARN] parse error: " << e.what() << "\n";
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] " << e.what() << "\n";
            return 1;
        }
    }

    engine.set_pipeline_complete();
    engine.log_summary();

    std::cout << "pipeline complete — "
              << engine.messages_processed() << " messages processed\n";
    std::cout << "REST server running at http://localhost:" << rest_port
              << " — press Ctrl+C to exit\n";

    pause();
    return 0;
}