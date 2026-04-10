#pragma once
#include <optional>
#include "itch/messages.h"
#include "itch/message_buffer.h"

namespace itch {

class MessageParser {
public:
    // Returns parsed message, nullopt for out-of-scope type codes.
    // Returns nullopt (not throw) on unknown type — discard is correct per spec.
    // Throws std::runtime_error on malformed input (truncated buffer etc).
    std::optional<ParsedMessage> parse(const MessageBuffer& buf);
};

} // namespace itch