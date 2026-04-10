// snapshot_publisher.cpp
#include "snapshot_publisher.h"

namespace itch {

void SnapshotPublisher::publish(std::shared_ptr<SystemSnapshot> snapshot) {
    snapshot_.store(std::move(snapshot));
}

std::shared_ptr<SystemSnapshot> SnapshotPublisher::current() const {
    return snapshot_.load();
}

} // namespace itch