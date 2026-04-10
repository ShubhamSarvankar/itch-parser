#include <iostream>
#include <cstdlib>
#include <string>
#include "feed_reader.h"
#include "parser.h"
#include "book_engine.h"
#include "snapshot_publisher.h"
#include "rest_server.h"
#include <unistd.h>

static std::string env(const char* key, const char* fallback) {
    const char* val = std::getenv(key);
    return val ? val : fallback;
}

int main() {
    std::string itch_file        = env("ITCH_FILE",          "data/sample.bin");
    uint16_t    rest_port        = static_cast<uint16_t>(
                                     std::stoi(env("REST_PORT", "8080")));
    uint64_t    snapshot_interval= std::stoull(env("SNAPSHOT_INTERVAL", "1000"));

    itch::SnapshotPublisher publisher;
    itch::OrderBookEngine   engine(publisher);
    engine.set_snapshot_interval(snapshot_interval);

    itch::RestServer rest(publisher, rest_port);
    rest.start();

    std::cout << "itch-parser starting\n";
    std::cout << "reading: " << itch_file << "\n";
    std::cout << "REST:    http://localhost:" << rest_port << "\n";

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

    engine.set_pipeline_complete();
    engine.log_summary();

    std::cout << "pipeline complete — "
              << engine.messages_processed() << " messages processed\n";

    // Keep REST server alive for queries after pipeline completes
    std::cout << "pipeline complete — "
              << engine.messages_processed() << " messages processed\n";
    std::cout << "REST server running at http://localhost:" << rest_port
              << " — press Ctrl+C to exit\n";

    // Block until interrupted
    pause();

    return 0;
}