#pragma once

// ────────────────────────────────────────────────────────────────────────────
// market_data.hpp  —  L2 Depth View & Publisher
//
// DESIGN
// ------
// The L2Publisher is a *derived view* of the order book.  It consumes the
// same engine Event stream (Task 2.1) and maintains aggregated depth:
//
//   • One Quantity sum per price level per side — NOT per order.
//   • Emits a full L2Snapshot on demand (or on first subscribe).
//   • Emits incremental L2Delta updates after every event that changes depth.
//
// SNAPSHOT-THEN-DELTAS CONTRACT
// ------------------------------
//  1. A client subscribes and receives a L2Snapshot with sequence number S.
//  2. All subsequent L2Deltas have seq > S, monotonically increasing.
//  3. A delta's `prev_seq` field equals the seq of the last message the client
//     should have seen.  If client's last-seen seq != delta.prev_seq, the
//     client has a *gap* and must re-request a snapshot.
//
//       Client invariant:  apply delta iff (my_seq == delta.prev_seq)
//                          else: request fresh snapshot
//
// GAP DETECTION
// -------------
//  seq numbers on both snapshots and deltas are gapless and monotonically
//  increasing (drawn from the same counter).  A client comparing its locally
//  tracked `last_seq` against `delta.prev_seq` can detect any missed message
//  without a heartbeat.
//
// INCREMENTAL UPDATE SEMANTICS
// ----------------------------
//  Each L2Delta carries the NEW total quantity for a level.
//  A new_qty == 0 means the level was removed entirely.
//  There is at most one delta per price level per event batch.
//
// ALLOCATION
// ----------
//  L2Publisher owns no heap beyond its two std::map<Price,Quantity> tables
//  and the order_meta_ cache.
// ────────────────────────────────────────────────────────────────────────────

#include "orderbook/events.hpp"
#include "orderbook/types.hpp"

#include <cstdint>
#include <map>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <optional>
#include <functional>

namespace orderbook {

// ── Public Trade ─────────────────────────────────────────────────────────────
// An anonymized execution record emitted when two orders match.
// Strict privacy boundary: contains NO internal OrderIds or participant UserIds.
struct PublicTrade {
    uint64_t  seq{0};              // trade sequence number (or engine event seq)
    Timestamp timestamp{0};        // engine matching timestamp (nanoseconds)
    Price     price{0};            // execution price
    Quantity  quantity{0};         // executed quantity
    Side      aggressor_side{Side::Buy}; // side of the incoming taker (Buy = buyer crossed spread)
    uint8_t   _pad[7]{};           // alignment padding

    bool operator==(const PublicTrade& other) const {
        return seq == other.seq &&
               timestamp == other.timestamp &&
               price == other.price &&
               quantity == other.quantity &&
               aggressor_side == other.aggressor_side;
    }
};
static_assert(sizeof(PublicTrade) == 40, "PublicTrade must be 40 bytes");
static_assert(std::is_trivially_copyable_v<PublicTrade>);

// ── Best Bid / Offer (Level 1 Quote) ─────────────────────────────────────────
// Top of book state: best bid price/qty and best ask price/qty.
// Anonymized: zero participant or order identifiers.
struct BBO {
    uint64_t             seq{0};
    std::optional<Price> bid_price{std::nullopt};
    Quantity             bid_qty{0};
    std::optional<Price> ask_price{std::nullopt};
    Quantity             ask_qty{0};

    bool is_empty()     const { return !bid_price.has_value() && !ask_price.has_value(); }
    bool has_bid()      const { return bid_price.has_value() && bid_qty > 0; }
    bool has_ask()      const { return ask_price.has_value() && ask_qty > 0; }
    bool is_two_sided() const { return has_bid() && has_ask(); }

    std::optional<Price> spread() const {
        if (is_two_sided()) return *ask_price - *bid_price;
        return std::nullopt;
    }

    std::optional<double> mid_price() const {
        if (is_two_sided()) return static_cast<double>(*bid_price + *ask_price) / 2.0;
        return std::nullopt;
    }

    bool operator==(const BBO&) const = default;
};

// ── Privacy & Sanitization Boundary Compile-Time Verifications ───────────────
template <typename T>
concept HasOrderId = requires(T t) { t.order_id; } ||
                     requires(T t) { t.taker_id; } ||
                     requires(T t) { t.maker_id; };

template <typename T>
concept HasUserId = requires(T t) { t.user_id; } ||
                    requires(T t) { t.client_order_id; };

static_assert(!HasOrderId<PublicTrade>, "PublicTrade must NOT contain order identifiers");
static_assert(!HasUserId<PublicTrade>,  "PublicTrade must NOT contain user identifiers");
static_assert(!HasOrderId<BBO>,         "BBO must NOT contain order identifiers");
static_assert(!HasUserId<BBO>,          "BBO must NOT contain user identifiers");

// ── L2 Level ─────────────────────────────────────────────────────────────────
// A single price level in the aggregated depth view.
struct L2Level {
    Price    price{0};
    Quantity qty{0};   // total resting quantity at this price

    bool operator==(const L2Level&) const = default;
};

// ── L2 Snapshot ──────────────────────────────────────────────────────────────
// Full depth snapshot.  bids are descending by price; asks ascending.
// seq is the publisher's current sequence number at snapshot time.
//
// A client that receives a snapshot should:
//   1. Replace its local depth image with snapshot.bids / snapshot.asks.
//   2. Record snapshot.seq as its `last_seq`.
//   3. Accept deltas only when delta.prev_seq == last_seq.
struct L2Snapshot {
    uint64_t             seq{0};       // publisher sequence at snapshot time
    std::vector<L2Level> bids;         // descending by price, qty > 0
    std::vector<L2Level> asks;         // ascending  by price, qty > 0

    bool operator==(const L2Snapshot&) const = default;
};

// ── L2 Delta ─────────────────────────────────────────────────────────────────
// A single incremental update to one price level on one side.
//
//  seq      — this update's sequence number
//  prev_seq — the sequence number of the previous message the client must
//              have applied.  If client.last_seq != prev_seq → GAP detected.
//  side     — which side changed
//  price    — the price level affected
//  new_qty  — the new total quantity at that level AFTER this update.
//              new_qty == 0 means the level no longer exists (removed).
struct L2Delta {
    uint64_t seq{0};
    uint64_t prev_seq{0};
    Side     side{Side::Buy};
    Price    price{0};
    Quantity new_qty{0};   // 0 → level removed

    bool operator==(const L2Delta&) const = default;
};

static_assert(!HasOrderId<L2Delta>,     "L2Delta must NOT contain order identifiers");
static_assert(!HasUserId<L2Delta>,      "L2Delta must NOT contain user identifiers");
static_assert(!HasOrderId<L2Snapshot>,  "L2Snapshot must NOT contain order identifiers");
static_assert(!HasUserId<L2Snapshot>,   "L2Snapshot must NOT contain user identifiers");
static_assert(!HasOrderId<L2Level>,     "L2Level must NOT contain order identifiers");
static_assert(!HasUserId<L2Level>,      "L2Level must NOT contain user identifiers");

// ── L2 Update ────────────────────────────────────────────────────────────────
// A batch of deltas produced by processing one engine Event.
// Most events produce 0–2 deltas (one per side touched).
// A fill that sweeps N levels produces up to N bid/ask deltas.
using L2Update = std::vector<L2Delta>;

// ── L2Publisher ──────────────────────────────────────────────────────────────
//
// Stateful consumer of the engine Event stream.
// Call process(event) for every Event emitted by the engine (in sequence order).
// It returns an L2Update (possibly empty) for each event.
//
// Protocol:
//   L2Publisher pub;
//   L2Snapshot  snap = pub.snapshot();            // initial full image
//
//   engine.add(order, [&](const Event& e) {
//       auto deltas = pub.process(e);
//       for (auto& d : deltas) distribute(d);     // push to subscribers
//   });
//
//   // New client connects — gets consistent starting point:
//   L2Snapshot fresh = pub.snapshot();
//
class L2Publisher {
public:
    // ── Snapshot ─────────────────────────────────────────────────────────────
    // Returns a consistent snapshot of the current aggregated depth.
    // The snapshot's seq is the publisher's last-emitted seq so a client
    // can immediately begin applying subsequent deltas.
    L2Snapshot snapshot() const {
        L2Snapshot snap;
        snap.seq = last_seq_;

        // bids descending
        for (auto it = bid_depth_.rbegin(); it != bid_depth_.rend(); ++it) {
            if (it->second > 0)
                snap.bids.push_back({it->first, it->second});
        }
        // asks ascending
        for (auto& [price, qty] : ask_depth_) {
            if (qty > 0)
                snap.asks.push_back({price, qty});
        }
        return snap;
    }

    // ── Process ──────────────────────────────────────────────────────────────
    // Consume one engine event and return depth deltas (may be empty).
    // Must be called in gapless sequence-number order.
    L2Update process(const Event& ev) {
        switch (ev.tag()) {
            case EventTag::OrderAccepted:
                return on_accepted(ev.accepted);
            case EventTag::Fill:
                return on_fill(ev.fill);
            case EventTag::OrderCancelled:
                return on_cancelled(ev.cancelled);
            // Rejected / CancelRejected / Reserved → no depth change
            default:
                // Flush any pending resting order if a non-fill breaks the sequence
                return flush_pending_rested();
        }
    }

    // ── Accessors (for testing and downstream consumers) ─────────────────────
    uint64_t                         last_seq()   const { return last_seq_; }
    const std::map<Price, Quantity>& bid_depth()  const { return bid_depth_; }
    const std::map<Price, Quantity>& ask_depth()  const { return ask_depth_; }

    // ── BBO (Level 1 Quote) ──────────────────────────────────────────────────
    BBO bbo() const {
        BBO out;
        out.seq = last_seq_;
        if (!bid_depth_.empty()) {
            auto it = bid_depth_.rbegin();
            out.bid_price = it->first;
            out.bid_qty   = it->second;
        }
        if (!ask_depth_.empty()) {
            auto it = ask_depth_.begin();
            out.ask_price = it->first;
            out.ask_qty   = it->second;
        }
        return out;
    }

    // ── Public Trade Stream ──────────────────────────────────────────────────
    const std::vector<PublicTrade>& trades() const { return trades_; }

    std::optional<PublicTrade> last_trade() const {
        return trades_.empty() ? std::nullopt : std::optional<PublicTrade>(trades_.back());
    }

    std::vector<PublicTrade> drain_trades() {
        std::vector<PublicTrade> out = std::move(trades_);
        trades_.clear();
        return out;
    }

    // ── Reset ─────────────────────────────────────────────────────────────────
    void reset() {
        bid_depth_.clear();
        ask_depth_.clear();
        order_meta_.clear();
        trades_.clear();
        pending_ = {};
        has_pending_ = false;
        last_seq_ = 0;
    }

private:
    // Aggregated resting quantity per price level.
    std::map<Price, Quantity> bid_depth_;   // key: price, ascending
    std::map<Price, Quantity> ask_depth_;   // key: price, ascending

    // Anonymized public trade history emitted on match events.
    std::vector<PublicTrade> trades_;

    // Lightweight per-resting-order metadata (needed for cancel depth adjustment).
    struct OrderMeta {
        Side     side{Side::Buy};
        Price    price{0};
        Quantity resting_qty{0};  // open qty when the order rested
    };
    std::unordered_map<OrderId, OrderMeta> order_meta_;

    // Pending resting context: after OrderAccepted we may see Fill events.
    // When the next non-Fill event arrives (or process() ends) we finalize
    // the resting qty for depth tracking.
    //
    // price and original_qty are NOT available from EventOrderAccepted alone
    // (events are intentionally minimal).  They must be injected by the
    // integration layer via notify_order_accepted() before or immediately
    // after the OrderAccepted event is processed.
    struct PendingAccepted {
        OrderId   order_id{0};
        Side      side{Side::Buy};
        OrderType order_type{OrderType::Limit};
        Price     price{0};        // limit price — set via notify_order_accepted()
        Quantity  original_qty{0}; // original submitted quantity — set via notify_order_accepted()
        Quantity  qty_filled{0};   // accumulated fill qty so far
    };
    PendingAccepted pending_{};
    bool            has_pending_{false};

    uint64_t last_seq_{0};

    // ── Helpers ───────────────────────────────────────────────────────────────

    uint64_t next_seq() { return ++last_seq_; }

    L2Delta make_delta(Side side, Price price, Quantity new_qty) {
        uint64_t prev = last_seq_;
        uint64_t seq  = next_seq();
        return L2Delta{seq, prev, side, price, new_qty};
    }

    // Adjust aggregate at (side, price) by -delta_qty.
    // Returns {new_qty, emitted_delta_opt}.
    std::pair<Quantity, bool> reduce_depth(Side side, Price price, Quantity delta_qty) {
        auto& depth = (side == Side::Buy) ? bid_depth_ : ask_depth_;
        auto it = depth.find(price);
        if (it == depth.end()) return {0, false};

        if (it->second <= delta_qty) {
            depth.erase(it);
            return {0, true};
        }
        it->second -= delta_qty;
        return {it->second, true};
    }

    void add_to_depth(Side side, Price price, Quantity qty) {
        auto& depth = (side == Side::Buy) ? bid_depth_ : ask_depth_;
        depth[price] += qty;
    }

    Quantity get_depth(Side side, Price price) const {
        const auto& depth = (side == Side::Buy) ? bid_depth_ : ask_depth_;
        auto it = depth.find(price);
        return (it != depth.end()) ? it->second : 0;
    }

    // ── flush_pending_rested ──────────────────────────────────────────────────
    // Commit any pending resting order into the depth aggregates.
    // Called when we receive an event that cannot be a Fill for the pending order.
    L2Update flush_pending_rested(L2Update deltas = {}) {
        if (!has_pending_) return deltas;
        has_pending_ = false;
        const auto& pa = pending_;

        // Market orders never rest.
        if (pa.order_type == OrderType::Market) return deltas;

        // The resting quantity is original_qty minus what was filled.
        Quantity rest_qty = (pa.original_qty > pa.qty_filled)
                                ? (pa.original_qty - pa.qty_filled)
                                : 0;
        if (rest_qty == 0) return deltas;

        // Commit to depth
        add_to_depth(pa.side, pa.price, rest_qty);
        Quantity new_qty = get_depth(pa.side, pa.price);
        deltas.push_back(make_delta(pa.side, pa.price, new_qty));

        // Register in order_meta_ so we can handle a future cancel
        order_meta_[pa.order_id] = {pa.side, pa.price, rest_qty};

        return deltas;
    }

    // ── Event handlers ────────────────────────────────────────────────────────

    L2Update on_accepted(const EventOrderAccepted& ev) {
        if (has_pending_ && pending_.order_id == ev.order_id) {
            pending_.side       = ev.side;
            pending_.order_type = ev.order_type;
            return {};
        }

        // Flush any prior pending before recording the new one.
        auto deltas = flush_pending_rested();

        pending_ = PendingAccepted{
            .order_id     = ev.order_id,
            .side         = ev.side,
            .order_type   = ev.order_type,
            .price        = 0,          // filled in from on_fill or external hint
            .original_qty = 0,          // filled in from on_fill
            .qty_filled   = 0
        };
        has_pending_ = true;
        return deltas;
    }

    L2Update on_fill(const EventFill& ev) {
        L2Update deltas;

        // Determine maker side from pending taker context.
        Side maker_side = Side::Buy;  // will be overridden below
        if (has_pending_ && ev.taker_id == pending_.order_id) {
            // The pending order is the taker.
            maker_side = (pending_.side == Side::Buy) ? Side::Sell : Side::Buy;
            pending_.qty_filled  += ev.quantity;

            // We learn the taker's original_qty lazily via fills.
            // We can't know it until the order rests (or doesn't).
            // That's fine — we accumulate qty_filled and compute rest at flush.
            // But we need original_qty for the rest calculation.
            // It's provided via notify_order_qty() or inferred from fill context.
            // For now we rely on notify_order_original_qty() being called by
            // the integration harness.  In the test harness we call it directly.
        } else {
            // Taker not matching our pending — the fill is for a maker we track.
            // We must infer maker side from bid_depth_/ask_depth_.
            // If maker_id is in order_meta_ we know its side exactly.
            auto it = order_meta_.find(ev.maker_id);
            if (it != order_meta_.end()) {
                maker_side = it->second.side;
                // Reduce resting qty for this maker in meta
                if (it->second.resting_qty > ev.quantity)
                    it->second.resting_qty -= ev.quantity;
                else
                    order_meta_.erase(it);
            } else {
                // Fallback: infer from which depth table contains the fill price
                if (bid_depth_.count(ev.price)) maker_side = Side::Buy;
                else                             maker_side = Side::Sell;
            }
        }

        // Always: reduce maker's aggregate at fill price.
        auto [new_qty, changed] = reduce_depth(maker_side, ev.price, ev.quantity);
        if (changed) {
            deltas.push_back(make_delta(maker_side, ev.price, new_qty));
        }

        // Record anonymized public trade (aggressor side is the incoming taker's side)
        Side aggressor_side = (maker_side == Side::Buy) ? Side::Sell : Side::Buy;
        trades_.push_back(PublicTrade{
            .seq            = ev.header.seq,
            .timestamp      = ev.header.engine_time,
            .price          = ev.price,
            .quantity       = ev.quantity,
            .aggressor_side = aggressor_side,
            ._pad           = {}
        });

        return deltas;
    }

    L2Update on_cancelled(const EventOrderCancelled& ev) {
        // Flush any pending resting order (shouldn't precede a cancel normally,
        // but be defensive).
        auto deltas = flush_pending_rested();

        auto it = order_meta_.find(ev.order_id);
        if (it == order_meta_.end()) {
            // Unknown order — already fully filled or market order.
            return deltas;
        }

        const auto& meta = it->second;
        // Lazy cancel policy: adjust the aggregate immediately using remaining_qty.
        auto [new_qty, changed] = reduce_depth(meta.side, meta.price, ev.remaining_qty);
        order_meta_.erase(it);

        if (changed) {
            deltas.push_back(make_delta(meta.side, meta.price, new_qty));
        }
        return deltas;
    }

public:
    // ── Integration helpers (called by Engine wrapper / test harness) ─────────
    //
    // Because EventOrderAccepted intentionally omits price and quantity (to keep
    // the event schema minimal and binary-stable), the integration layer must
    // inject these fields via notify_order_accepted() so the publisher can
    // correctly compute the resting quantity when the order finally rests.
    //
    // Call order:
    //   1. Call notify_order_accepted(id, side, price, qty) BEFORE or IMMEDIATELY
    //      AFTER calling process(EventOrderAccepted).
    //   2. Call process() for each subsequent Fill event.
    //   3. The resting delta is emitted automatically on the next
    //      non-Fill event (or can be forced via flush_pending()).

    // Inject price + original_qty for the pending accepted order.
    // Must be called once per accepted limit order; safe to call before or
    // immediately after process(EventOrderAccepted).
    void notify_order_accepted(OrderId id, Side side, Price price, Quantity qty) {
        if (has_pending_ && pending_.order_id == id) {
            pending_.side         = side;   // reinforce in case on_accepted already set it
            pending_.price        = price;
            pending_.original_qty = qty;
        } else {
            flush_pending_rested();
            pending_ = PendingAccepted{
                .order_id     = id,
                .side         = side,
                .order_type   = OrderType::Limit,
                .price        = price,
                .original_qty = qty,
                .qty_filled   = 0
            };
            has_pending_ = true;
        }
    }

    // Backward-compat shim: sets only original_qty (price must already be
    // known from on_accepted or a prior notify_order_accepted call).
    // Prefer notify_order_accepted() for new code.
    void notify_order_original_qty(OrderId id, Quantity qty) {
        if (has_pending_ && pending_.order_id == id) {
            pending_.original_qty = qty;
        }
    }

    // Force-flush any pending resting order and return the resulting deltas.
    // Call this after the last event in a command batch when you cannot rely
    // on a subsequent event to trigger the lazy flush automatically.
    L2Update flush_pending() {
        return flush_pending_rested();
    }

    // Called by the engine wrapper after an order is confirmed resting.
    // Shortcut: immediately commits the resting order to depth without waiting
    // for the next event to trigger the lazy flush.  This path is used when the
    // integration layer has direct knowledge of the final resting quantity (e.g.
    // after book_.add() returns).  Either call this OR rely on flush_pending().
    void notify_rested(OrderId id, Side side, Price price, Quantity resting_qty) {
        // Discard any pending we have for this id — it is being committed now.
        if (has_pending_ && pending_.order_id == id) {
            has_pending_ = false;
        }

        add_to_depth(side, price, resting_qty);
        Quantity new_qty = get_depth(side, price);
        L2Delta d = make_delta(side, price, new_qty);
        order_meta_[id] = {side, price, resting_qty};
        // We don't return deltas here — caller must call process() on events.
        // This is a secondary path for frameworks that know resting status directly.
        (void)d;
    }

    // Called after an order is confirmed fully filled (remove from meta).
    void notify_filled(OrderId id) {
        order_meta_.erase(id);
    }
};

// ── apply_delta ───────────────────────────────────────────────────────────────
// Apply one L2Delta to a snapshot image, updating seq.
// Returns false (gap detected) if snap.seq != delta.prev_seq.
inline bool apply_delta(L2Snapshot& snap, const L2Delta& delta) {
    if (snap.seq != delta.prev_seq) return false;  // GAP

    auto& levels = (delta.side == Side::Buy) ? snap.bids : snap.asks;
    bool  descending = (delta.side == Side::Buy);

    // Find existing level
    auto it = std::find_if(levels.begin(), levels.end(),
        [&](const L2Level& l) { return l.price == delta.price; });

    if (it != levels.end()) {
        if (delta.new_qty == 0) {
            levels.erase(it);
        } else {
            it->qty = delta.new_qty;
        }
    } else if (delta.new_qty > 0) {
        // Insert in sorted order
        L2Level newlvl{delta.price, delta.new_qty};
        if (descending) {
            auto pos = std::lower_bound(levels.begin(), levels.end(), newlvl,
                [](const L2Level& a, const L2Level& b) { return a.price > b.price; });
            levels.insert(pos, newlvl);
        } else {
            auto pos = std::lower_bound(levels.begin(), levels.end(), newlvl,
                [](const L2Level& a, const L2Level& b) { return a.price < b.price; });
            levels.insert(pos, newlvl);
        }
    }

    snap.seq = delta.seq;
    return true;
}

// ── apply_update ──────────────────────────────────────────────────────────────
// Apply a full L2Update batch.  Returns false on first gap.
inline bool apply_update(L2Snapshot& snap, const L2Update& update) {
    for (const auto& d : update) {
        if (!apply_delta(snap, d)) return false;
    }
    return true;
}

// ── MarketDataFeed ───────────────────────────────────────────────────────────
//
// Unified public market data feed pipeline.
// Bridges the engine's private Event stream to public subscribers:
//  - Emits PublicTrade on matched executions (EventFill)
//  - Emits BBO updates whenever top-of-book (price or qty) changes
//  - Emits L2Delta incremental depth updates
//  - Emits L2Snapshot on subscription
//
// Strict boundary: all outputs are strictly sanitized public structures.
class MarketDataFeed {
public:
    MarketDataFeed() = default;

    // Process an engine event, notifying callbacks of public data updates.
    template<typename OnTrade, typename OnBBO, typename OnL2Update>
    void process(const Event& ev, OnTrade&& on_trade, OnBBO&& on_bbo, OnL2Update&& on_l2) {
        BBO prev_bbo = pub_.bbo();

        L2Update deltas = pub_.process(ev);

        if (ev.tag() == EventTag::Fill) {
            auto trade = pub_.last_trade();
            if (trade) on_trade(*trade);
        }

        if (!deltas.empty()) {
            on_l2(deltas);
        }

        BBO curr_bbo = pub_.bbo();
        if (!(prev_bbo == curr_bbo)) {
            on_bbo(curr_bbo);
        }
    }

    // Flush any pending resting order and notify callbacks of any BBO/L2 changes.
    template<typename OnBBO, typename OnL2Update>
    void flush_pending(OnBBO&& on_bbo, OnL2Update&& on_l2) {
        BBO prev_bbo = pub_.bbo();
        L2Update deltas = pub_.flush_pending();
        if (!deltas.empty()) {
            on_l2(deltas);
        }
        BBO curr_bbo = pub_.bbo();
        if (!(prev_bbo == curr_bbo)) {
            on_bbo(curr_bbo);
        }
    }

    void notify_order_accepted(OrderId id, Side side, Price price, Quantity qty) {
        pub_.notify_order_accepted(id, side, price, qty);
    }

    L2Snapshot snapshot() const { return pub_.snapshot(); }
    BBO bbo() const { return pub_.bbo(); }
    const L2Publisher& publisher() const { return pub_; }
    L2Publisher& publisher() { return pub_; }
    void reset() { pub_.reset(); }

private:
    L2Publisher pub_;
};

} // namespace orderbook
