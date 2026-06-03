#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#ifdef ITCH_HAVE_DENSE_MAP
#include <ankerl/unordered_dense.h>
#endif
#include "itch/messages.h"
#include "itch/order_record.h"
#include "itch/order_book.h"
#include "itch/instrument_info.h"
#include "itch/snapshot.h"

namespace itch {

#ifdef ITCH_HAVE_DENSE_MAP
template <class K, class V> using fast_map = ankerl::unordered_dense::map<K, V>;
#else
template <class K, class V> using fast_map = std::unordered_map<K, V>;
#endif

class SnapshotPublisher;

class OrderBookEngine {
public:
    explicit OrderBookEngine(SnapshotPublisher& publisher);

    void apply(const ParsedMessage& msg);
    void set_pipeline_complete();
    void log_summary() const;

    void set_snapshot_interval(uint64_t interval) {
        snapshot_interval_ = interval;
    }

    // Accessors for testing
    const OrderBook*      get_book(uint16_t stock_locate) const;
    const OrderRecord*    get_order(uint64_t order_ref) const;
    const InstrumentInfo* get_instrument(uint16_t stock_locate) const;
    uint64_t skipped_unknown_ref() const { return skipped_unknown_ref_; }
    uint64_t corrupt_messages()    const { return corrupt_messages_; }
    uint64_t messages_processed()  const { return messages_processed_; }
    bool     pipeline_complete()   const { return pipeline_complete_; }

    void register_instrument(const InstrumentInfo& info);

    // Test accessor — triggers a snapshot build from current state
    std::shared_ptr<SystemSnapshot> build_snapshot_for_test() const;

private:
    void handle(const AddOrderMsg& m);
    void handle(const AddOrderMPIDMsg& m);
    void handle(const OrderExecutedMsg& m);
    void handle(const OrderExecutedPriceMsg& m);
    void handle(const OrderCancelMsg& m);
    void handle(const OrderDeleteMsg& m);
    void handle(const OrderReplaceMsg& m);
    void handle(const StockDirectoryMsg& m);
    void handle(const StockTradingActionMsg& m);

    void add_order_impl(uint16_t stock_locate, uint64_t order_ref,
                        char side, uint32_t shares, Price price,
                        uint64_t timestamp, std::string_view mpid);
    void remove_shares(OrderBook& book, char side, Price price,
                       uint32_t shares, bool full_removal);
    OrderBook& get_or_create_book(uint16_t stock_locate);
    void stamp_book(uint16_t stock_locate, uint64_t timestamp);
    void maybe_publish_snapshot();
    std::shared_ptr<SystemSnapshot> build_snapshot() const;

    fast_map<uint64_t, OrderRecord>    order_index_;
    fast_map<uint16_t, OrderBook>      books_;
    fast_map<uint16_t, InstrumentInfo> instruments_;

    uint64_t messages_processed_{0};
    uint64_t skipped_unknown_ref_{0};
    uint64_t corrupt_messages_{0};
    uint64_t snapshot_interval_{1000};
    bool     pipeline_complete_{false};

    SnapshotPublisher& publisher_;
};

} // namespace itch