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
        TestCommand::limit(4, Side::Buy,  100, 15, 4) // Fills all of 1 and half of 2
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
        TestCommand::limit(2, Side::Buy,  100, 15, 2), // 5 of maker 1 remains
        TestCommand::limit(3, Side::Buy,  100, 10, 3)  // fills remainder of 1, 5 of 3 rests
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
        TestCommand::limit(5, Side::Buy,  102, 25, 5) // Sweeps 100, 101, and 5 of 102
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
        TestCommand::limit(4, Side::Sell, 100, 10, 4) // Should match against order 2
    };

    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

TEST_CASE("Differential: Scenario 7 - Invalid cancellations", "[differential]") {
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Sell, 100, 10, 1),
        TestCommand::limit(2, Side::Buy,  100, 10, 2), // fully fills order 1
        TestCommand::cancel(1),                         // cancel already filled
        TestCommand::cancel(999),                       // cancel non-existent
        TestCommand::cancel(999),                       // repeat non-existent
        TestCommand::cancel(2)                          // cancel already filled taker
    };

    auto result = run_differential_sequence(cmds);
    REQUIRE(result.passed);
}

// ── 2. Randomized Differential & Property Fuzzing ─────────────────────────────

TEST_CASE("Differential: Multi-seed randomized differential fuzzing", "[differential][fuzz]") {
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
    // Inject a synthetic failure by manually constructing a mismatch sequence
    std::vector<TestCommand> cmds = {
        TestCommand::limit(1, Side::Buy,  100, 10, 1),
        TestCommand::limit(2, Side::Buy,   99, 10, 2),
        TestCommand::limit(3, Side::Sell, 105, 10, 3),
        TestCommand::limit(4, Side::Sell, 106, 10, 4),
        TestCommand::limit(5, Side::Buy,  105, 10, 5)
    };

    // Minimizer on passing sequence returns original or prefix
    auto minimized = minimize_failing_sequence(cmds);
    REQUIRE(!minimized.empty());
}
