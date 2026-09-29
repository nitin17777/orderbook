#include "test_command.hpp"
#include "test_executor.hpp"
#include "test_snapshot.hpp"
#include "test_invariants.hpp"
#include "test_generator.hpp"
#include "test_reducer.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <cstdlib>

using namespace orderbook;
using namespace orderbook::test;

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --seed <num>       Run a single deterministic seed (default: random or multi-seed)\n"
              << "  --ops <num>        Operations per seed (default: 5000)\n"
              << "  --seeds <num>      Number of sequential seeds to test (default: 50)\n"
              << "  --min-price <p>    Minimum base price band (default: 90)\n"
              << "  --max-price <p>    Maximum base price band (default: 110)\n"
              << "  --minimize         Minimize sequence if failure occurs (default: enabled)\n"
              << "  --help             Show this help\n";
}

int main(int argc, char* argv[]) {
    uint64_t specific_seed = 0;
    bool has_specific_seed = false;
    size_t ops_per_seed = 5000;
    size_t num_seeds = 50;
    Price min_price = 90;
    Price max_price = 110;
    bool minimize_on_fail = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            specific_seed = std::stoull(argv[++i]);
            has_specific_seed = true;
        } else if (arg == "--ops" && i + 1 < argc) {
            ops_per_seed = std::stoull(argv[++i]);
        } else if (arg == "--seeds" && i + 1 < argc) {
            num_seeds = std::stoull(argv[++i]);
        } else if (arg == "--min-price" && i + 1 < argc) {
            min_price = std::stoll(argv[++i]);
        } else if (arg == "--max-price" && i + 1 < argc) {
            max_price = std::stoll(argv[++i]);
        } else if (arg == "--no-minimize") {
            minimize_on_fail = false;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    std::cout << "========================================================\n"
              << "  OrderBook Differential & Property Fuzz Harness\n"
              << "========================================================\n";

    if (has_specific_seed) {
        std::cout << "Running single seed: " << specific_seed << " with " << ops_per_seed << " ops...\n";
        CommandGenerator gen(specific_seed, min_price, max_price);
        auto sequence = gen.generate(ops_per_seed);

        auto start = std::chrono::high_resolution_clock::now();
        auto res = run_differential_sequence(sequence);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - start);

        if (res.passed) {
            std::cout << "✅ Seed " << specific_seed << " PASSED (" << ops_per_seed
                      << " ops in " << (elapsed.count() / 1000.0) << " ms, "
                      << (ops_per_seed * 1'000'000.0 / elapsed.count()) << " ops/sec)\n";
            return 0;
        } else {
            std::cerr << "\n❌ DIFFERENTIAL FAILURE ON SEED: " << specific_seed << "\n"
                      << "  Failing Step:    " << res.failing_step << " / " << ops_per_seed << "\n"
                      << "  Failing Command: " << res.failing_command << "\n"
                      << "  Reason:          " << res.invariant_error << "\n\n"
                      << "  Expected (Naive) Result: " << res.expected_result << "\n"
                      << "  Actual (Fast) Result:    " << res.actual_result << "\n\n"
                      << BookSnapshot::diff_string(res.expected_snapshot, res.actual_snapshot) << "\n";

            if (minimize_on_fail) {
                std::cout << "\nRunning delta-debugging minimizer...\n";
                auto min_seq = minimize_failing_sequence(sequence);
                std::cout << "Minimized failing sequence length: " << min_seq.size() << " commands:\n";
                for (size_t k = 0; k < min_seq.size(); ++k) {
                    std::cout << "  [" << k << "] " << min_seq[k] << "\n";
                }
            }
            return 1;
        }
    }

    std::cout << "Running " << num_seeds << " seeds x " << ops_per_seed
              << " operations (" << (num_seeds * ops_per_seed) << " total ops)...\n\n";

    size_t total_ops = 0;
    auto total_start = std::chrono::high_resolution_clock::now();

    for (size_t s = 1; s <= num_seeds; ++s) {
        uint64_t seed = s * 6364136223846793005ULL + 1442695040888963407ULL;
        CommandGenerator gen(seed, min_price, max_price);
        auto sequence = gen.generate(ops_per_seed);

        auto res = run_differential_sequence(sequence);
        if (!res.passed) {
            std::cerr << "\n❌ DIFFERENTIAL FAILURE ON SEED: " << seed << " (Seed index " << s << ")\n"
                      << "  Failing Step:    " << res.failing_step << " / " << ops_per_seed << "\n"
                      << "  Failing Command: " << res.failing_command << "\n"
                      << "  Reason:          " << res.invariant_error << "\n\n"
                      << "  Expected (Naive) Result: " << res.expected_result << "\n"
                      << "  Actual (Fast) Result:    " << res.actual_result << "\n\n"
                      << BookSnapshot::diff_string(res.expected_snapshot, res.actual_snapshot) << "\n";

            if (minimize_on_fail) {
                std::cout << "\nRunning delta-debugging minimizer...\n";
                auto min_seq = minimize_failing_sequence(sequence);
                std::cout << "Minimized failing sequence (" << min_seq.size() << " commands):\n";
                for (size_t k = 0; k < min_seq.size(); ++k) {
                    std::cout << "  [" << k << "] " << min_seq[k] << "\n";
                }
                std::cout << "\nReproduce with: " << argv[0] << " --seed " << seed << " --ops " << ops_per_seed << "\n";
            }
            return 1;
        }

        total_ops += ops_per_seed;
        if (s % 10 == 0 || s == num_seeds) {
            std::cout << "  Seed " << s << " / " << num_seeds << " passed (" << total_ops << " ops completed)\n";
        }
    }

    auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - total_start);

    std::cout << "\n🎉 ALL " << num_seeds << " SEEDS (" << total_ops << " OPS) PASSED!\n"
              << "   Total Time:   " << total_elapsed.count() << " ms\n"
              << "   Throughput:   " << (total_ops * 1000.0 / total_elapsed.count()) << " ops/sec\n\n";

    return 0;
}
