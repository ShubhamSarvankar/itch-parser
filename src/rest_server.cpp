#include "rest_server.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <cctype>

namespace itch {

using json = nlohmann::json;

// Helpers

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

static std::string trading_state_description(char state) {
    switch (state) {
        case 'T': return "Trading";
        case 'H': return "Halted";
        case 'P': return "Paused";
        case 'Q': return "Quotation only";
        default:  return "Unknown";
    }
}

// Constructor / destructor

RestServer::RestServer(SnapshotPublisher& publisher, uint16_t port)
    : publisher_(publisher)
    , port_(port)
    , server_(std::make_unique<httplib::Server>())
{
    setup_routes();
}

RestServer::~RestServer() {
    stop();
}

void RestServer::start() {
    thread_ = std::thread([this]() {
        server_->listen("0.0.0.0", port_);
    });
}

void RestServer::stop() {
    server_->stop();
    if (thread_.joinable()) thread_.join();
}

// Route setup

void RestServer::setup_routes() {

    // GET /status
    server_->Get("/status", [this](const httplib::Request&,
                                    httplib::Response& res) {
        auto snap = publisher_.current();
        json j;
        if (!snap) {
            j = {
                {"messages_processed",  0},
                {"instruments_tracked", 0},
                {"snapshot_timestamp",  0},
                {"pipeline_complete",   false}
            };
        } else {
            j = {
                {"messages_processed",  snap->messages_processed},
                {"instruments_tracked", snap->books.size()},
                {"snapshot_timestamp",  snap->snapshot_timestamp},
                {"pipeline_complete",   snap->pipeline_complete}
            };
        }
        res.set_content(j.dump(), "application/json");
    });

    // GET /instruments
    server_->Get("/instruments", [this](const httplib::Request&,
                                         httplib::Response& res) {
        auto snap = publisher_.current();
        if (!snap) {
            res.status = 503;
            res.set_content(
                json{{"error", "snapshot not yet available"}}.dump(),
                "application/json");
            return;
        }

        json instruments = json::array();
        for (const auto& [symbol, book] : snap->books) {
            json entry;
            entry["symbol"]                   = book.symbol;
            entry["trading_state"]            = std::string(1, book.trading_state);
            entry["trading_state_description"]= trading_state_description(
                                                    book.trading_state);
            instruments.push_back(entry);
        }

        json j;
        j["count"]       = instruments.size();
        j["instruments"] = instruments;
        res.set_content(j.dump(), "application/json");
    });

    // GET /book/:symbol
    server_->Get("/book/:symbol", [this](const httplib::Request& req,
                                          httplib::Response& res) {
        auto snap = publisher_.current();
        if (!snap) {
            res.status = 503;
            res.set_content(
                json{{"error", "snapshot not yet available"}}.dump(),
                "application/json");
            return;
        }

        // Depth parameter validation
        int depth = 10;
        if (req.has_param("depth")) {
            const std::string& d = req.get_param_value("depth");
            try {
                std::size_t pos;
                depth = std::stoi(d, &pos);
                if (pos != d.size()) throw std::invalid_argument("trailing");
            } catch (...) {
                res.status = 400;
                res.set_content(
                    json{{"error", "depth must be an integer"}}.dump(),
                    "application/json");
                return;
            }
            if (depth < 1) {
                res.status = 400;
                res.set_content(
                    json{{"error", "depth must be at least 1"}}.dump(),
                    "application/json");
                return;
            }
            if (depth > 50) {
                res.status = 400;
                res.set_content(
                    json{{"error", "depth must not exceed 50"}}.dump(),
                    "application/json");
                return;
            }
        }

        // Case-insensitive symbol lookup
        std::string requested = to_lower(req.path_params.at("symbol"));
        const OrderBookSnapshot* book_snap = nullptr;
        for (const auto& [sym, book] : snap->books) {
            if (to_lower(sym) == requested) {
                book_snap = &book;
                break;
            }
        }

        if (!book_snap) {
            res.status = 404;
            res.set_content(
                json{{"error", "instrument not found: " +
                      req.path_params.at("symbol")}}.dump(),
                "application/json");
            return;
        }

        json bids = json::array();
        int bid_count = 0;
        for (const auto& level : book_snap->bids) {
            if (bid_count >= depth) break;
            bids.push_back({
                {"price",        level.price},
                {"total_shares", level.total_shares},
                {"order_count",  level.order_count}
            });
            ++bid_count;
        }

        json asks = json::array();
        int ask_count = 0;
        for (const auto& level : book_snap->asks) {
            if (ask_count >= depth) break;
            asks.push_back({
                {"price",        level.price},
                {"total_shares", level.total_shares},
                {"order_count",  level.order_count}
            });
            ++ask_count;
        }

        json j;
        j["symbol"]               = book_snap->symbol;
        j["trading_state"]        = std::string(1, book_snap->trading_state);
        j["last_update_timestamp"]= book_snap->last_update_timestamp > 0
                                        ? json(book_snap->last_update_timestamp)
                                        : json(nullptr);
        j["bid_count"]            = bid_count;
        j["ask_count"]            = ask_count;
        j["bids"]                 = bids;
        j["asks"]                 = asks;
        res.set_content(j.dump(), "application/json");
    });

    // GET /book/:symbol/top
    server_->Get("/book/:symbol/top", [this](const httplib::Request& req,
                                              httplib::Response& res) {
        auto snap = publisher_.current();
        if (!snap) {
            res.status = 503;
            res.set_content(
                json{{"error", "snapshot not yet available"}}.dump(),
                "application/json");
            return;
        }

        std::string requested = to_lower(req.path_params.at("symbol"));
        const OrderBookSnapshot* book_snap = nullptr;
        for (const auto& [sym, book] : snap->books) {
            if (to_lower(sym) == requested) {
                book_snap = &book;
                break;
            }
        }

        if (!book_snap) {
            res.status = 404;
            res.set_content(
                json{{"error", "instrument not found: " +
                      req.path_params.at("symbol")}}.dump(),
                "application/json");
            return;
        }

        json best_bid = nullptr;
        json best_ask = nullptr;
        json spread   = nullptr;

        if (!book_snap->bids.empty()) {
            const auto& b = book_snap->bids.front();
            best_bid = {
                {"price",        b.price},
                {"total_shares", b.total_shares},
                {"order_count",  b.order_count}
            };
        }

        if (!book_snap->asks.empty()) {
            const auto& a = book_snap->asks.front();
            best_ask = {
                {"price",        a.price},
                {"total_shares", a.total_shares},
                {"order_count",  a.order_count}
            };
        }

        if (!book_snap->bids.empty() && !book_snap->asks.empty()) {
            // Fixed-point spread: convert display doubles back to raw,
            // subtract, convert once — avoids IEEE 754 noise
            uint32_t ask_raw = static_cast<uint32_t>(
                book_snap->asks.front().price * 10000.0 + 0.5);
            uint32_t bid_raw = static_cast<uint32_t>(
                book_snap->bids.front().price * 10000.0 + 0.5);
            spread = (ask_raw - bid_raw) / 10000.0;
        }

        json j;
        j["symbol"]               = book_snap->symbol;
        j["trading_state"]        = std::string(1, book_snap->trading_state);
        j["last_update_timestamp"]= book_snap->last_update_timestamp > 0
                                        ? json(book_snap->last_update_timestamp)
                                        : json(nullptr);
        j["best_bid"]             = best_bid;
        j["best_ask"]             = best_ask;
        j["spread"]               = spread;
        res.set_content(j.dump(), "application/json");
    });
}

} // namespace itch