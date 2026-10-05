#pragma once

// ────────────────────────────────────────────────────────────────────────────
// test_market_data.hpp  —  L2 Depth Reconstruction Harness
//
// Provides two entry points:
//
//  run_l2_reconstruction_check(sequence)
//    Replay a command sequence through the Engine (with event sink).
//    At the end compare:
//      A) The L2Snapshot reconstructed by applying snapshot + all deltas
//      B) A fresh L2Snapshot scanned directly from the finished book
//    Returns a result struct with pass/fail + diff details.
//
//  run_l2_aggregate_check(sequence)
//    After each command, verify that the per-level aggregate quantities
//    stored inside OrderBook and FastOrderBook exactly match a direct
//    scan of all resting open quantities in the deques.
// ────────────────────────────────────────────────────────────────────────────

#include "test_command.hpp"
#include "test_snapshot.hpp"

#include "orderbook/engine.hpp"
#include "orderbook/market_data.hpp"
#include "orderbook/book.hpp"
#include "orderbook/fast_book.hpp"

#include <string>
#include <sstream>
#include <vector>
#include <map>

namespace orderbook::test {

// ── Result types ──────────────────────────────────────────────────────────────

struct L2ReconstructResult {
    bool   passed{true};
    size_t failing_step{0};
    TestCommand failing_command{};
    std::string error{};

    // The two depth images being compared (on failure)
    L2Snapshot reconstructed;   // snapshot + applied deltas
    L2Snapshot ground_truth;    // fresh scan from the book
};

struct L2AggregateResult {
    bool   passed{true};
    size_t failing_step{0};
    TestCommand failing_command{};
    std::string error{};
};

// ── Ground-truth L2 snapshot from a live OrderBook ───────────────────────────
// Scan the book's deques directly (ignoring per-level aggregate fields)
// to produce an authoritative L2 depth image for comparison.
inline L2Snapshot ground_truth_l2(const OrderBook& book) {
    L2Snapshot snap;
    snap.seq = 0; // seq not meaningful for comparison — we zero it

    // Bids descending (book.bids() is already std::greater<Price>)
    for (const auto& [price, deque] : book.bids()) {
        Quantity total = 0;
        for (const auto& o : deque)
            total += o.open_quantity();
        if (total > 0)
            snap.bids.push_back({price, total});
    }
    // Asks ascending
    for (const auto& [price, deque] : book.asks()) {
        Quantity total = 0;
        for (const auto& o : deque)
            total += o.open_quantity();
        if (total > 0)
            snap.asks.push_back({price, total});
    }
    return snap;
}

// ── Ground-truth L2 from FastOrderBook ───────────────────────────────────────
inline L2Snapshot ground_truth_l2(const FastOrderBook& book) {
    L2Snapshot snap;
    snap.seq = 0;

    // Bids descending
    for (Price p = MAX_PRICE; p >= MIN_PRICE; --p) {
        const auto& q = book.bids().queue(p);
        if (q.empty()) continue;
        Quantity total = 0;
        for (OrderId id : q) {
            const Order* o = book.pool().get(id);
            if (o && !o->is_terminal())
                total += o->open_quantity();
        }
        if (total > 0)
            snap.bids.push_back({p, total});
    }
    // Asks ascending
    for (Price p = MIN_PRICE; p <= MAX_PRICE; ++p) {
        const auto& q = book.asks().queue(p);
        if (q.empty()) continue;
        Quantity total = 0;
        for (OrderId id : q) {
            const Order* o = book.pool().get(id);
            if (o && !o->is_terminal())
                total += o->open_quantity();
        }
        if (total > 0)
            snap.asks.push_back({p, total});
    }
    return snap;
}

// ── Zero-seq snapshot comparison (ignore seq field) ──────────────────────────
inline bool l2_depth_equal(const L2Snapshot& a, const L2Snapshot& b) {
    return a.bids == b.bids && a.asks == b.asks;
}

inline std::string l2_diff(const L2Snapshot& reconstructed, const L2Snapshot& truth) {
    std::ostringstream ss;
    ss << "=== RECONSTRUCTED (snapshot+deltas) ===\n";
    ss << "  BIDS (" << reconstructed.bids.size() << "):\n";
    for (auto& l : reconstructed.bids)
        ss << "    @" << l.price << " qty=" << l.qty << "\n";
    ss << "  ASKS (" << reconstructed.asks.size() << "):\n";
    for (auto& l : reconstructed.asks)
        ss << "    @" << l.price << " qty=" << l.qty << "\n";

    ss << "=== GROUND TRUTH (book scan) ===\n";
    ss << "  BIDS (" << truth.bids.size() << "):\n";
    for (auto& l : truth.bids)
        ss << "    @" << l.price << " qty=" << l.qty << "\n";
    ss << "  ASKS (" << truth.asks.size() << "):\n";
    for (auto& l : truth.asks)
        ss << "    @" << l.price << " qty=" << l.qty << "\n";
    return ss.str();
}

// ── L2 aggregate correctness (per-level qty matches direct scan) ──────────────
inline std::string check_book_aggregates(const OrderBook& book) {
    for (auto it = book.bids().rbegin(); it != book.bids().rend(); ++it) {
        Price price = it->first;
        Quantity expected = 0;
        for (const auto& o : it->second)
            expected += o.open_quantity();
        Quantity got = book.bid_level_qty(price);
        if (got != expected) {
            std::ostringstream ss;
            ss << "OrderBook bid aggregate mismatch @" << price
               << ": stored=" << got << " scan=" << expected;
            return ss.str();
        }
    }
    for (const auto& [price, deque] : book.asks()) {
        Quantity expected = 0;
        for (const auto& o : deque)
            expected += o.open_quantity();
        Quantity got = book.ask_level_qty(price);
        if (got != expected) {
            std::ostringstream ss;
            ss << "OrderBook ask aggregate mismatch @" << price
               << ": stored=" << got << " scan=" << expected;
            return ss.str();
        }
    }
    return {};
}

inline std::string check_fast_book_aggregates(const FastOrderBook& book) {
    // Bids
    for (Price p = MIN_PRICE; p <= MAX_PRICE; ++p) {
        const auto& q = book.bids().queue(p);
        Quantity expected = 0;
        for (OrderId id : q) {
            const Order* o = book.pool().get(id);
            if (o && !o->is_terminal())
                expected += o->open_quantity();
        }
        Quantity got = book.bid_level_qty(p);
        if (got != expected) {
            std::ostringstream ss;
            ss << "FastOrderBook bid aggregate mismatch @" << p
               << ": stored=" << got << " scan=" << expected;
            return ss.str();
        }
    }
    // Asks
    for (Price p = MIN_PRICE; p <= MAX_PRICE; ++p) {
        const auto& q = book.asks().queue(p);
        Quantity expected = 0;
        for (OrderId id : q) {
            const Order* o = book.pool().get(id);
            if (o && !o->is_terminal())
                expected += o->open_quantity();
        }
        Quantity got = book.ask_level_qty(p);
        if (got != expected) {
            std::ostringstream ss;
            ss << "FastOrderBook ask aggregate mismatch @" << p
               << ": stored=" << got << " scan=" << expected;
            return ss.str();
        }
    }
    return {};
}

// ── Primary harness: L2 reconstruction via snapshot + deltas ─────────────────
//
// Protocol exercised:
//  1. Take an initial snapshot (empty book).
//  2. For each command:
//      a. Execute through Engine (which emits Events into a VectorSink).
//      b. Feed each event into L2Publisher::process() → collect deltas.
//      c. Apply deltas to the client's running snapshot image.
//      d. Compare client's image against a fresh ground-truth scan.
//  3. Also check gap-detection: every delta's prev_seq must equal client's
//     last_seq before the delta is applied.
inline L2ReconstructResult run_l2_reconstruction_check(
        const std::vector<TestCommand>& sequence,
        const std::string& tmp_log = "tmp_l2_test.log") {

    // Engine + Publisher
    Engine engine(tmp_log);
    L2Publisher pub;

    // Client's running depth image (starts from initial snapshot)
    L2Snapshot client_image = pub.snapshot();

    for (size_t i = 0; i < sequence.size(); ++i) {
        const auto& cmd = sequence[i];

        VectorSink sink;

        if (cmd.kind == CommandKind::LimitOrder || cmd.kind == CommandKind::MarketOrder) {
            Order o{};
            o.id        = cmd.id;
            o.side      = cmd.side;
            o.type      = (cmd.kind == CommandKind::LimitOrder)
                              ? OrderType::Limit : OrderType::Market;
            o.status    = OrderStatus::Accepted;
            o.tif       = cmd.tif;
            o.price     = cmd.price;
            o.quantity  = cmd.quantity;
            o.filled    = 0;
            o.timestamp = cmd.timestamp;

            engine.add(o, sink);

        } else {
            engine.cancel(cmd.id, INVALID_USER_ID, sink);
        }

        // Feed events to publisher and accumulate deltas into client image
        for (const auto& ev : sink.events) {
            // Before or with OrderAccepted, tell publisher the price and qty
            if (ev.tag() == EventTag::OrderAccepted &&
                    (cmd.kind == CommandKind::LimitOrder ||
                     cmd.kind == CommandKind::MarketOrder)) {
                pub.notify_order_accepted(cmd.id, cmd.side, cmd.price, cmd.quantity, cmd.tif);
            }

            L2Update updates = pub.process(ev);
            for (const auto& delta : updates) {
                // Gap check
                if (client_image.seq != delta.prev_seq) {
                    std::ostringstream ss;
                    ss << "GAP detected: client_seq=" << client_image.seq
                       << " delta.prev_seq=" << delta.prev_seq;
                    return L2ReconstructResult{
                        .passed = false,
                        .failing_step = i,
                        .failing_command = cmd,
                        .error = ss.str(),
                        .reconstructed = client_image,
                        .ground_truth = ground_truth_l2(engine.book())
                    };
                }
                apply_delta(client_image, delta);
            }
        }

        // Flush any resting order left from this command
        L2Update flush_deltas = pub.flush_pending();
        for (const auto& delta : flush_deltas) {
            if (client_image.seq != delta.prev_seq) {
                std::ostringstream ss;
                ss << "GAP detected on flush: client_seq=" << client_image.seq
                   << " delta.prev_seq=" << delta.prev_seq;
                return L2ReconstructResult{
                    .passed = false,
                    .failing_step = i,
                    .failing_command = cmd,
                    .error = ss.str(),
                    .reconstructed = client_image,
                    .ground_truth = ground_truth_l2(engine.book())
                };
            }
            apply_delta(client_image, delta);
        }

        // Compare client's reconstructed depth against ground-truth scan of book.
        // Zero out seq for comparison (ground truth doesn't have a seq).
        L2Snapshot truth = ground_truth_l2(engine.book());
        L2Snapshot reconstructed_zeroed = client_image;
        reconstructed_zeroed.seq = 0;

        if (!l2_depth_equal(reconstructed_zeroed, truth)) {
            std::ostringstream err;
            err << "L2 depth mismatch after step " << i << "\n"
                << l2_diff(reconstructed_zeroed, truth);
            return L2ReconstructResult{
                .passed = false,
                .failing_step = i,
                .failing_command = cmd,
                .error = err.str(),
                .reconstructed = reconstructed_zeroed,
                .ground_truth = truth
            };
        }
    }

    return L2ReconstructResult{
        .passed = true,
        .failing_step = 0,
        .failing_command = {},
        .error = {},
        .reconstructed = client_image,
        .ground_truth = ground_truth_l2(engine.book())
    };
}

// ── Per-level aggregate correctness check ────────────────────────────────────
// Verifies bid_level_qty / ask_level_qty match a direct scan after each step.
inline L2AggregateResult run_l2_aggregate_check(
        const std::vector<TestCommand>& sequence) {

    OrderBook naive;
    FastOrderBook fast;

    for (size_t i = 0; i < sequence.size(); ++i) {
        const auto& cmd = sequence[i];

        if (cmd.kind == CommandKind::LimitOrder || cmd.kind == CommandKind::MarketOrder) {
            Order o{};
            o.id        = cmd.id;
            o.side      = cmd.side;
            o.type      = (cmd.kind == CommandKind::LimitOrder)
                              ? OrderType::Limit : OrderType::Market;
            o.status    = OrderStatus::Accepted;
            o.price     = cmd.price;
            o.quantity  = cmd.quantity;
            o.filled    = 0;
            o.timestamp = cmd.timestamp;
            naive.add(o);
            fast.add(o);
        } else {
            naive.cancel(cmd.id);
            fast.cancel(cmd.id);
        }

        auto naive_err = check_book_aggregates(naive);
        if (!naive_err.empty()) {
            return L2AggregateResult{false, i, cmd, "OrderBook: " + naive_err};
        }

        auto fast_err = check_fast_book_aggregates(fast);
        if (!fast_err.empty()) {
            return L2AggregateResult{false, i, cmd, "FastOrderBook: " + fast_err};
        }
    }

    return L2AggregateResult{true};
}

} // namespace orderbook::test
