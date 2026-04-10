#pragma once
#include <optional>
#include <string>
#include "itch/message_buffer.h"

namespace itch {

class FileFeedReader {
public:
    explicit FileFeedReader(const std::string& path);
    ~FileFeedReader();

    // Returns the next message buffer, or nullopt at clean EOF.
    // Throws std::runtime_error on truncated frame or read error.
    std::optional<MessageBuffer> next_message();

    bool is_open() const { return fd_ >= 0; }

private:
    int fd_{-1};
};

} // namespace itch