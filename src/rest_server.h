#pragma once
#include <cstdint>
#include <memory>
#include <thread>
#include "snapshot_publisher.h"

namespace httplib { class Server; }

namespace itch {

class RestServer {
public:
    RestServer(SnapshotPublisher& publisher, uint16_t port);
    ~RestServer();

    void start();
    void stop();

private:
    void setup_routes();

    SnapshotPublisher&             publisher_;
    uint16_t                       port_;
    std::unique_ptr<httplib::Server> server_;
    std::thread                    thread_;
};

} // namespace itch