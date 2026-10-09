#include <catch2/catch_test_macros.hpp>

#include "test_command.hpp"
#include "test_executor.hpp"
#include "test_snapshot.hpp"
#include "test_invariants.hpp"
#include "test_generator.hpp"
#include "test_reducer.hpp"

using namespace orderbook;
using namespace orderbook::test;

// ── 1. Manually Constructed Scenarios ─────────────────────────────────────────

TEST_CASE("Differential: Scenario 1 - Single resting limit orders", "[differential]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy,  100, 10, 1),
        TestCommand::limit(2, Side::Sell, 105, 10, 2)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 2 - Same-price FIFO execution", "[differential]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1),
        TestCommand::limit(2, Side::Sell, 100, 10, 2),
        TestCommand::limit(3, Side::Sell, 100, 10, 3),
        TestCommand::limit(4, Side::Buy,  100, 15, 4)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 3 - Simple exact match", "[differential]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 25, 1),
        TestCommand::limit(2, Side::Buy,  100, 25, 2)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 4 - Partial fills", "[differential]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 20, 1),
        TestCommand::limit(2, Side::Buy,  100, 15, 2),
        TestCommand::limit(3, Side::Buy,  100, 10, 3)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 5 - Multi-level sweeps", "[differential]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1),
        TestCommand::limit(2, Side::Sell, 101, 10, 2),
        TestCommand::limit(3, Side::Sell, 102, 10, 3),
        TestCommand::limit(4, Side::Sell, 103, 10, 4),
        TestCommand::limit(5, Side::Buy,  102, 25, 5)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 6 - Active order cancellations", "[differential]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy, 100, 10, 1),
        TestCommand::limit(2, Side::Buy, 100, 20, 2),
        TestCommand::limit(3, Side::Buy,  99, 15, 3),
        TestCommand::cancel(1),
        TestCommand::limit(4, Side::Sell, 100, 10, 4)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 7 - Invalid cancellations", "[differential]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1),
        TestCommand::limit(2, Side::Buy,  100, 10, 2),
        TestCommand::cancel(1),
        TestCommand::cancel(999),
        TestCommand::cancel(999),
        TestCommand::cancel(2)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 8 - IOC partial fill and zero resting tail", "[differential][tif]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 5, 1),
        TestCommand::limit(2, Side::Sell, 101, 5, 2),
        TestCommand::ioc(3, Side::Buy, 100, 12, 3)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 9 - FOK atomic fill-or-kill decision", "[differential][tif]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 5, 1),
        TestCommand::limit(2, Side::Sell, 101, 5, 2),
        TestCommand::fok(3, Side::Buy, 105, 15, 3),
        TestCommand::fok(4, Side::Buy, 100, 10, 4),
        TestCommand::fok(5, Side::Buy, 105, 10, 5)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 10 - FOK with lazy cancellations", "[differential][tif]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy, 100, 10, 1),
        TestCommand::limit(2, Side::Buy, 100, 10, 2),
        TestCommand::limit(3, Side::Buy,  99, 10, 3),
        TestCommand::cancel(1),
        TestCommand::fok(4, Side::Sell, 99, 25, 4),
        TestCommand::fok(5, Side::Sell, 99, 20, 5)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 11 - Market order IOC vs FOK semantics", "[differential][tif]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1),
        TestCommand::limit(2, Side::Sell, 105, 10, 2),
        TestCommand::market(3, Side::Buy, 30, 3, TimeInForce::FOK),
        TestCommand::market(4, Side::Buy, 15, 4, TimeInForce::IOC),
        TestCommand::market(5, Side::Buy, 5, 5, TimeInForce::FOK)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

// ── Task 5.2: Modify Differential Scenarios ───────────────────────────────────

TEST_CASE("Differential: Scenario 12 - Modify retain path (reduce qty at same price)", "[differential][modify]") {
    // Reduce order 1 from 10 to 6 at same price: retains queue position.
    // Sell 20 should fill: order1(6), order2(10), order3(4 of 10).
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy, 100, 10, 1),
        TestCommand::limit(2, Side::Buy, 100, 10, 2),
        TestCommand::limit(3, Side::Buy, 100, 10, 3),
        TestCommand::modify(1, 100, 6, 4),
        TestCommand::limit(4, Side::Sell, 100, 20, 5)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 13 - Modify lose path (increase qty at same price)", "[differential][modify]") {
    // Increase order 1 from 5 to 15 at same price: loses queue position (goes to back).
    // Sell 5 should fill order 2 first.
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy, 100, 5, 1),
        TestCommand::limit(2, Side::Buy, 100, 5, 2),
        TestCommand::modify(1, 100, 15, 3),
        TestCommand::limit(3, Side::Sell, 100, 5, 4)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 14 - Modify lose path (price change)", "[differential][modify]") {
    // Order 1 moves from price 105 to 106: loses position in 105 level.
    // Buy at 105 should fill order 2 only.
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 105, 10, 1),
        TestCommand::limit(2, Side::Sell, 105, 10, 2),
        TestCommand::modify(1, 106, 10, 3),
        TestCommand::limit(3, Side::Buy, 105, 10, 4)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 15 - Modify price crosses and triggers immediate fill", "[differential][modify]") {
    // Lower ask from 105 to 100: crosses resting bid at 100 -> immediate fill.
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy,  100, 10, 1),
        TestCommand::limit(2, Side::Sell, 105, 10, 2),
        TestCommand::modify(2, 100, 10, 3),
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 16 - Modify unknown order is a no-op", "[differential][modify]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy, 100, 10, 1),
        TestCommand::modify(999, 100, 5, 2),
        TestCommand::modify(1,   100, 3, 3),
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 17 - Modify followed by cancel", "[differential][modify]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy, 100, 20, 1),
        TestCommand::modify(1, 101, 15, 2),
        TestCommand::cancel(1),
        TestCommand::limit(2, Side::Sell, 101, 5, 3)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

// ── Task 5.3: Self-Trade Prevention (STP) Differential Scenarios ──────────────

TEST_CASE("Differential: Scenario 18 - STP direct crossing between same user", "[differential][stp]") {
    UserId user_a = 10;
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1, TimeInForce::GTC, user_a),
        TestCommand::limit(2, Side::Buy,  100, 10, 2, TimeInForce::GTC, user_a)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 19 - STP multi-level sweep across mixed users", "[differential][stp]") {
    UserId user_a = 1, user_b = 2, user_c = 3;
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1, TimeInForce::GTC, user_a),
        TestCommand::limit(2, Side::Sell, 101, 10, 2, TimeInForce::GTC, user_b),
        TestCommand::limit(3, Side::Sell, 102, 10, 3, TimeInForce::GTC, user_c),
        TestCommand::limit(4, Side::Buy,  102, 25, 4, TimeInForce::GTC, user_b)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 20 - STP FOK atomic evaluation", "[differential][stp]") {
    UserId user_a = 1, user_b = 2;
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 5, 1, TimeInForce::GTC, user_a),
        TestCommand::limit(2, Side::Sell, 100, 5, 2, TimeInForce::GTC, user_b),
        TestCommand::fok(3, Side::Buy, 100, 10, 3, user_b), // Needs 10, but 5 is own -> FOK kills
        TestCommand::fok(4, Side::Buy, 100, 5, 4, user_b)   // Needs 5, fills user_a (5) -> succeeds
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 21 - STP Modify crossing into own resting order", "[differential][stp]") {
    UserId user_a = 99;
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy,  100, 10, 1, TimeInForce::GTC, user_a),
        TestCommand::limit(2, Side::Sell, 105, 10, 2, TimeInForce::GTC, user_a),
        TestCommand::modify(2, 100, 10, 3, user_a)
    };
    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

// ── 2. Randomized Differential & Property Fuzzing ─────────────────────────────

TEST_CASE("Differential: Multi-seed randomized differential fuzzing (with modify)", "[differential][fuzz]") {
    constexpr size_t NUM_SEEDS = 30;
    constexpr size_t OPS_PER_SEED = 500;

    for (uint64_t seed = 1; seed <= NUM_SEEDS; ++seed) {
        CommandGenerator generator(seed * 7919, 90, 110);
        auto sequence = generator.generate(OPS_PER_SEED);

        auto result = run_differential_sequence(sequence);
        if (!result.passed) {
            UNSCOPED_INFO("Failing Seed: " << (seed * 7919));
            UNSCOPED_INFO("Failing Step: " << result.failing_step);
            UNSCOPED_INFO("Failing Command: " << result.failing_command);
            UNSCOPED_INFO("Invariant Error: " << result.invariant_error);
            UNSCOPED_INFO(BookSnapshot::diff_string(result.expected_snapshot, result.actual_snapshot));
        }
        REQUIRE(result.passed);
    }
}

TEST_CASE("Differential: Sequence Minimizer reduces failing sequences", "[differential][reducer]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy,  100, 10, 1),
        TestCommand::limit(2, Side::Buy,   99, 10, 2),
        TestCommand::limit(3, Side::Sell, 105, 10, 3),
        TestCommand::limit(4, Side::Sell, 106, 10, 4),
        TestCommand::limit(5, Side::Buy,  105, 10, 5)
    };
    auto minimized = minimize_failing_sequence(cmds);
    REQUIRE(!minimized.empty());
}
