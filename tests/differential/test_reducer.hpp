#pragma once

#include "test_command.hpp"
#include "test_executor.hpp"
#include "test_snapshot.hpp"
#include "test_invariants.hpp"

#include <vector>
#include <iostream>

namespace orderbook::test {

struct FuzzRunResult {
    bool passed{true};
    size_t failing_step{0};
    TestCommand failing_command{};
    CommandResult expected_result{};
    CommandResult actual_result{};
    BookSnapshot expected_snapshot{};
    BookSnapshot actual_snapshot{};
    std::string invariant_error{};
};

inline FuzzRunResult run_differential_sequence(const std::vector<TestCommand>& sequence) {
    OrderBook naive;
    FastOrderBook fast;

    try {
        for (size_t i = 0; i < sequence.size(); ++i) {
            const auto& cmd = sequence[i];

            auto res_naive = execute_command(naive, cmd);
            auto res_fast  = execute_command(fast, cmd);

            if (res_naive != res_fast) {
                return FuzzRunResult{
                    .passed = false,
                    .failing_step = i,
                    .failing_command = cmd,
                    .expected_result = res_naive,
                    .actual_result = res_fast,
                    .expected_snapshot = snapshot(naive),
                    .actual_snapshot = snapshot(fast),
                    .invariant_error = "CommandResult mismatch"
                };
            }

            auto snap_naive = snapshot(naive);
            auto snap_fast  = snapshot(fast);

            if (snap_naive != snap_fast) {
                return FuzzRunResult{
                    .passed = false,
                    .failing_step = i,
                    .failing_command = cmd,
                    .expected_result = res_naive,
                    .actual_result = res_fast,
                    .expected_snapshot = snap_naive,
                    .actual_snapshot = snap_fast,
                    .invariant_error = "BookSnapshot mismatch"
                };
            }

            auto inv_naive = verify_invariants(snap_naive);
            if (!inv_naive.ok) {
                return FuzzRunResult{
                    .passed = false,
                    .failing_step = i,
                    .failing_command = cmd,
                    .expected_result = res_naive,
                    .actual_result = res_fast,
                    .expected_snapshot = snap_naive,
                    .actual_snapshot = snap_fast,
                    .invariant_error = "Naive invariant failed: " + inv_naive.message
                };
            }

            auto inv_fast = verify_invariants(snap_fast);
            if (!inv_fast.ok) {
                return FuzzRunResult{
                    .passed = false,
                    .failing_step = i,
                    .failing_command = cmd,
                    .expected_result = res_naive,
                    .actual_result = res_fast,
                    .expected_snapshot = snap_naive,
                    .actual_snapshot = snap_fast,
                    .invariant_error = "Fast invariant failed: " + inv_fast.message
                };
            }
        }
    } catch (const std::exception& e) {
        return FuzzRunResult{
            .passed = false,
            .failing_step = 0,
            .failing_command = sequence.empty() ? TestCommand{} : sequence.front(),
            .invariant_error = std::string("Unhandled exception during execution: ") + e.what()
        };
    }

    return FuzzRunResult{.passed = true};
}

// Delta-debugging reducer for failing command sequences
inline std::vector<TestCommand> minimize_failing_sequence(const std::vector<TestCommand>& original) {
    auto initial_run = run_differential_sequence(original);
    if (initial_run.passed) return original; // Doesn't fail

    // 1. Truncate to the failing prefix
    std::vector<TestCommand> current(original.begin(), original.begin() + initial_run.failing_step + 1);

    // 2. Iteratively attempt to remove commands from the prefix
    bool reduced = true;
    while (reduced && current.size() > 1) {
        reduced = false;
        for (size_t i = 0; i < current.size() - 1; ++i) { // Keep the last command which triggered the fail
            std::vector<TestCommand> candidate = current;
            candidate.erase(candidate.begin() + i);

            auto run = run_differential_sequence(candidate);
            if (!run.passed) {
                current = std::move(candidate);
                reduced = true;
                break; // Restart loop with smaller sequence
            }
        }
    }

    return current;
}

} // namespace orderbook::test
