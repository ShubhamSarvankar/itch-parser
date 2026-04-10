#include "feed_reader.h"
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <fcntl.h>

namespace itch {

FileFeedReader::FileFeedReader(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw std::runtime_error("FileFeedReader: cannot open file: " + path);
    }
}

FileFeedReader::~FileFeedReader() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

// Reads exactly n bytes into buf starting at offset. Returns false on clean
// EOF at the very first byte, throws on partial read or error.
static bool read_exact(int fd, uint8_t* buf, std::size_t n) {
    std::size_t total = 0;
    while (total < n) {
        ssize_t r = ::read(fd, buf + total, n - total);
        if (r == 0) {
            if (total == 0) return false;  // clean EOF before any bytes
            throw std::runtime_error("FileFeedReader: truncated read");
        }
        if (r < 0) {
            throw std::runtime_error("FileFeedReader: read error");
        }
        total += static_cast<std::size_t>(r);
    }
    return true;
}

std::optional<MessageBuffer> FileFeedReader::next_message() {
    // Read 2-byte big-endian length prefix
    uint8_t len_buf[2];
    if (!read_exact(fd_, len_buf, 2)) {
        return std::nullopt;  // clean EOF
    }

    uint16_t msg_len = static_cast<uint16_t>(
        (static_cast<uint16_t>(len_buf[0]) << 8) | len_buf[1]
    );

    if (msg_len == 0) {
        throw std::runtime_error("FileFeedReader: zero-length message frame");
    }

    MessageBuffer buf(msg_len);
    // Truncated frame after length prefix is always an error
    try {
        read_exact(fd_, buf.data(), msg_len);
    } catch (...) {
        throw std::runtime_error("FileFeedReader: truncated message frame");
    }

    return buf;
}

} // namespace itch