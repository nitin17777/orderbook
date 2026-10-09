#pragma once

#include "test_command.hpp"
#include "orderbook/fast_book.hpp" // For MIN_PRICE, MAX_PRICE

#include <random>
#include <vector>
#include <cstdint>

namespace orderbook::test {

class CommandGenerator {
public:
    explicit CommandGenerator(uint64_t seed, Price min_price = 95, Price max_price = 105)
        : rng_(seed)
        , min_price_(min_price)
        , max_price_(max_price)
    {}

    // Generate a single command with diverse edge-case probabilities
    TestCommand next() {
        ++current_timestamp_;
        
        // Operation distribution:
        // ~58% Limit Orders, ~12% Market Orders, ~18% Cancels, ~12% Modifies
        std::discrete_distribution<int> kind_dist({58, 12, 18, 12});
        int choice = kind_dist(rng_);

        if (choice == 2 && !submitted_ids_.empty()) {
            return generate_cancel();
        } else if (choice == 1) {
            return generate_market();
        } else if (choice == 3 && !active_ids_.empty()) {
            return generate_modify();
        } else {
            return generate_limit();
        }
    }

    // Generate a batch of N commands
    std::vector<TestCommand> generate(size_t count) {
        std::vector<TestCommand> batch;
        batch.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            batch.push_back(next());
        }
        return batch;
    }

    void reset(uint64_t seed) {
        rng_.seed(seed);
        next_order_id_ = 1;
        current_timestamp_ = 0;
        submitted_ids_.clear();
        active_ids_.clear();
    }

private:
    std::mt19937_64 rng_;
    Price min_price_;
    Price max_price_;
    OrderId next_order_id_{1};
    Timestamp current_timestamp_{0};
    std::vector<OrderId> submitted_ids_;
    std::vector<OrderId> active_ids_;

    Side random_side() {
        std::bernoulli_distribution dist(0.5);
        return dist(rng_) ? Side::Buy : Side::Sell;
    }

    Quantity random_quantity() {
        // Skewed: 50% small (1-5), 35% medium (6-20), 15% large sweep (50-200)
        std::discrete_distribution<int> qty_type({50, 35, 15});
        int type = qty_type(rng_);
        if (type == 0) {
            std::uniform_int_distribution<Quantity> dist(1, 5);
            return dist(rng_);
        } else if (type == 1) {
            std::uniform_int_distribution<Quantity> dist(6, 20);
            return dist(rng_);
        } else {
            std::uniform_int_distribution<Quantity> dist(50, 200);
            return dist(rng_);
        }
    }

    Price random_price() {
        // 80% clustered in active price band, 10% extreme low/high, 10% single hotspot price (FIFO stress)
        std::discrete_distribution<int> price_type({80, 10, 10});
        int type = price_type(rng_);
        if (type == 0) {
            std::uniform_int_distribution<Price> dist(min_price_, max_price_);
            return dist(rng_);
        } else if (type == 1) {
            // Extreme boundary within valid domain [MIN_PRICE, MAX_PRICE]
            std::bernoulli_distribution edge_dist(0.5);
            return edge_dist(rng_) ? MIN_PRICE : MAX_PRICE;
        } else {
            // Hotspot price to heavily test FIFO queuing
            return (min_price_ + max_price_) / 2;
        }
    }

    TimeInForce random_tif() {
        // 70% GTC, 15% IOC, 15% FOK
        std::discrete_distribution<int> tif_dist({70, 15, 15});
        int t = tif_dist(rng_);
        if (t == 0) return TimeInForce::GTC;
        if (t == 1) return TimeInForce::IOC;
        return TimeInForce::FOK;
    }

    UserId random_user() {
        // Mix of active user IDs (1..4) and unassigned/anonymous (0)
        std::uniform_int_distribution<UserId> dist(0, 4);
        return dist(rng_);
    }

    TestCommand generate_limit() {
        OrderId id = next_order_id_++;
        submitted_ids_.push_back(id);
        TimeInForce tif = random_tif();
        if (tif == TimeInForce::GTC) {
            active_ids_.push_back(id);
        }

        return TestCommand::limit(
            id,
            random_side(),
            random_price(),
            random_quantity(),
            current_timestamp_,
            tif,
            random_user()
        );
    }

    TestCommand generate_market() {
        OrderId id = next_order_id_++;
        submitted_ids_.push_back(id);

        // 80% IOC, 20% FOK for market orders
        std::bernoulli_distribution fok_dist(0.2);
        TimeInForce tif = fok_dist(rng_) ? TimeInForce::FOK : TimeInForce::IOC;

        return TestCommand::market(
            id,
            random_side(),
            random_quantity(),
            current_timestamp_,
            tif,
            random_user()
        );
    }

    TestCommand generate_cancel() {
        // Cancel edge cases:
        // 70% active order cancel
        // 15% arbitrary/already cancelled or filled historical order ID
        // 15% unknown non-existent ID
        std::discrete_distribution<int> cancel_type({70, 15, 15});
        int type = cancel_type(rng_);

        if (type == 0 && !active_ids_.empty()) {
            std::uniform_int_distribution<size_t> dist(0, active_ids_.size() - 1);
            size_t idx = dist(rng_);
            OrderId target = active_ids_[idx];
            // Remove from active list
            active_ids_[idx] = active_ids_.back();
            active_ids_.pop_back();
            return TestCommand::cancel(target);
        } else if (type == 1 && !submitted_ids_.empty()) {
            std::uniform_int_distribution<size_t> dist(0, submitted_ids_.size() - 1);
            return TestCommand::cancel(submitted_ids_[dist(rng_)]);
        } else {
            // High non-existent ID
            std::uniform_int_distribution<OrderId> dist(900'000, 999'999);
            return TestCommand::cancel(dist(rng_));
        }
    }

    TestCommand generate_modify() {
        // Pick a random active order to modify
        std::uniform_int_distribution<size_t> idx_dist(0, active_ids_.size() - 1);
        OrderId target = active_ids_[idx_dist(rng_)];

        Price new_price = random_price();
        Quantity new_qty = random_quantity();

        return TestCommand::modify(target, new_price, new_qty, current_timestamp_);
    }
};

} // namespace orderbook::test
