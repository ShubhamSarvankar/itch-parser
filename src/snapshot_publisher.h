// snapshot_publisher.h
#pragma once
#include <memory>
#include <atomic>
#include "itch/snapshot.h"

namespace itch {

class SnapshotPublisher {
public:
    void publish(std::shared_ptr<SystemSnapshot> snapshot);
    std::shared_ptr<SystemSnapshot> current() const;

private:
    std::atomic<std::shared_ptr<SystemSnapshot>> snapshot_{nullptr};
};

} // namespace itch