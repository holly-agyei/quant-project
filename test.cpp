// Tests for RiskGate: basic accept/reject, boundary cases, overflow handling,
// fail-closed behaviour, and a concurrent run that reconciles the stored
// position against an independent recount. Exits non-zero if anything fails.
//
//   g++ -std=c++20 -O2 -pthread test.cpp -o test
#include "risk_gate.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using namespace rg;

static int g_checks = 0;
static int g_failed = 0;
static int g_boundary_cases = 0;

static void expect(bool cond, const char* where) {
    ++g_checks;
    if (!cond) {
        ++g_failed;
        std::printf("  FAIL: %s\n", where);
    }
}

static Order ord(int64_t id, Side s, int64_t px, int64_t qty) {
    return Order{id, s, px, qty};
}

static void test_basic() {
    std::printf("basic accept/reject\n");
    RiskGate<16> gate;
    gate.configure(0, SymbolLimits{1000, 100 * PRICE_SCALE, 100, 1000});

    expect(accepted(gate.check(ord(0, Side::Buy, 100 * PRICE_SCALE, 10))),
           "valid order accepts");
    expect(gate.check(ord(0, Side::Buy, 100 * PRICE_SCALE, 101)) ==
               Decision::RejectSize,
           "oversized order rejects");
    expect(gate.check(ord(0, Side::Buy, 50 * PRICE_SCALE, 10)) ==
               Decision::RejectPrice,
           "out-of-band price rejects");

    gate.configure(0, SymbolLimits{1000, 100 * PRICE_SCALE, 2000, 1000});
    expect(gate.check(ord(0, Side::Buy, 100 * PRICE_SCALE, 1001)) ==
               Decision::RejectPosition,
           "over-limit position rejects");
}

// For each check, test the exact limit, one below, and one above, swept over a
// range of symbols and limit values so the cases are distinct. Reconfigure
// before each so the position starts at zero and only the check under test binds.
static void test_boundaries() {
    int before = g_failed;
    int cases = 0;
    std::printf("boundary cases\n");
    RiskGate<64> gate;

    for (int i = 0; i < 60; ++i) {
        const int64_t ref  = (int64_t)(50 + i) * PRICE_SCALE;
        const int64_t bps  = 100 + i * 10;
        const int64_t size = 100 + i;
        const int64_t poslim = 1000 + i * 7;
        const int64_t id = i % 64;

        gate.configure(id, SymbolLimits{poslim, ref, size, bps});
        expect(accepted(gate.check(ord(id, Side::Buy, ref, size))),
               "size == cap accepts"); ++cases;
        gate.configure(id, SymbolLimits{poslim, ref, size, bps});
        expect(gate.check(ord(id, Side::Buy, ref, size + 1)) ==
                   Decision::RejectSize, "size == cap+1 rejects"); ++cases;
        gate.configure(id, SymbolLimits{poslim, ref, size, bps});
        expect(accepted(gate.check(ord(id, Side::Buy, ref, size - 1))),
               "size == cap-1 accepts"); ++cases;

        // band = ref*bps/10000; hi and lo are the exact inclusive edges.
        const int64_t band = ref * bps / BPS_DENOM;
        const int64_t hi = ref + band, lo = ref - band;
        gate.configure(id, SymbolLimits{poslim, ref, size, bps});
        expect(accepted(gate.check(ord(id, Side::Buy, hi, 1))),
               "price == hi accepts"); ++cases;
        gate.configure(id, SymbolLimits{poslim, ref, size, bps});
        expect(gate.check(ord(id, Side::Buy, hi + 1, 1)) ==
                   Decision::RejectPrice, "price == hi+1 rejects"); ++cases;
        gate.configure(id, SymbolLimits{poslim, ref, size, bps});
        expect(accepted(gate.check(ord(id, Side::Buy, lo, 1))),
               "price == lo accepts"); ++cases;
        gate.configure(id, SymbolLimits{poslim, ref, size, bps});
        expect(gate.check(ord(id, Side::Buy, lo - 1, 1)) ==
                   Decision::RejectPrice, "price == lo-1 rejects"); ++cases;

        // Size cap set high so only the position limit binds here.
        gate.configure(id, SymbolLimits{poslim, ref, poslim + 10, bps});
        expect(accepted(gate.check(ord(id, Side::Buy, ref, poslim))),
               "position == limit accepts"); ++cases;
        gate.configure(id, SymbolLimits{poslim, ref, poslim + 10, bps});
        expect(gate.check(ord(id, Side::Buy, ref, poslim + 1)) ==
                   Decision::RejectPosition, "position == limit+1 rejects"); ++cases;
        gate.configure(id, SymbolLimits{poslim, ref, poslim + 10, bps});
        expect(accepted(gate.check(ord(id, Side::Sell, ref, poslim))),
               "position == -limit accepts"); ++cases;
        gate.configure(id, SymbolLimits{poslim, ref, poslim + 10, bps});
        expect(gate.check(ord(id, Side::Sell, ref, poslim + 1)) ==
                   Decision::RejectPosition, "position == -(limit+1) rejects"); ++cases;
    }

    g_boundary_cases = cases;
    std::printf("  %d cases, %s\n", cases, g_failed == before ? "ok" : "FAILED");
}

static void test_overflow() {
    int before = g_failed;
    std::printf("overflow handling\n");
    constexpr int64_t MAX = INT64_MAX;
    RiskGate<8> gate;

    // Fill the position near the top, then an order that would push it past
    // INT64_MAX must reject and leave the position untouched.
    gate.configure(0, SymbolLimits{MAX, 100 * PRICE_SCALE, MAX, 1000});
    expect(accepted(gate.check(ord(0, Side::Buy, 100 * PRICE_SCALE, MAX - 5))),
           "fill to MAX-5");
    expect(gate.check(ord(0, Side::Buy, 100 * PRICE_SCALE, 10)) ==
               Decision::RejectOverflow, "position add overflow rejects");
    expect(gate.position(0) == MAX - 5, "position unchanged after reject");

    gate.configure(1, SymbolLimits{MAX, 100 * PRICE_SCALE, MAX, 1000});
    expect(accepted(gate.check(ord(1, Side::Buy, 100 * PRICE_SCALE, MAX))),
           "order of MAX at the edge accepts");
    expect(gate.check(ord(1, Side::Buy, 100 * PRICE_SCALE, 1)) ==
               Decision::RejectOverflow, "one more overflows");

    // reference * bps overflows int64 -> reject rather than compute a bad band.
    gate.configure(2, SymbolLimits{MAX, MAX / 2, 100, BPS_DENOM});
    expect(gate.check(ord(2, Side::Buy, 100 * PRICE_SCALE, 1)) ==
               Decision::RejectOverflow, "price-band multiply overflow rejects");

    std::printf("  %s\n", g_failed == before ? "ok" : "FAILED");
}

static void test_fail_closed() {
    int before = g_failed;
    std::printf("fail-closed\n");
    RiskGate<32> gate;
    gate.configure(5, SymbolLimits{1000, 100 * PRICE_SCALE, 100, 1000});

    expect(gate.check(ord(-1, Side::Buy, 100 * PRICE_SCALE, 1)) ==
               Decision::RejectBadSymbol, "negative id rejects");
    expect(gate.check(ord(32, Side::Buy, 100 * PRICE_SCALE, 1)) ==
               Decision::RejectBadSymbol, "id == N rejects");
    expect(gate.check(ord(9999, Side::Buy, 100 * PRICE_SCALE, 1)) ==
               Decision::RejectBadSymbol, "id >> N rejects");
    expect(gate.check(ord(6, Side::Buy, 100 * PRICE_SCALE, 1)) ==
               Decision::RejectUninitialized, "unconfigured symbol rejects");

    std::printf("  %s\n", g_failed == before ? "ok" : "FAILED");
}

static long long g_concurrency_ops = 0;

// Several threads send orders at a few hot symbols. Each thread keeps a private
// tally of what it committed; afterwards we sum the tallies and compare to the
// gate's stored position. A double-count or lost update would make them differ.
static void test_concurrency() {
    std::printf("concurrency\n");
    constexpr int64_t NSYM = 32;
    constexpr int THREADS = 8;
    constexpr long long OPS_PER_THREAD = 1'500'000; // 8 * 1.5M = 12M total

    RiskGate<NSYM> gate;
    // Limits set wide so nothing rejects and every order commits.
    for (int64_t i = 0; i < NSYM; ++i)
        gate.configure(i, SymbolLimits{INT64_MAX / 4, 100 * PRICE_SCALE,
                                       1'000'000, 5000});

    std::vector<std::vector<int64_t>> local(THREADS,
                                            std::vector<int64_t>(NSYM, 0));
    std::atomic<long long> accepted_total{0};

    auto worker = [&](int tid) {
        std::mt19937_64 rng(0xC0FFEE + tid);
        long long acc = 0;
        for (long long k = 0; k < OPS_PER_THREAD; ++k) {
            uint64_t r = rng();
            // 80% of traffic on 4 symbols, the rest spread across the universe.
            int64_t sym = (r % 100 < 80) ? (r % 4) : (r % NSYM);
            Side side = (r & 0x100) ? Side::Buy : Side::Sell;
            int64_t qty = 1 + (r % 5);
            if (accepted(gate.check(ord(sym, side, 100 * PRICE_SCALE, qty)))) {
                local[tid][sym] += (side == Side::Buy) ? qty : -qty;
                ++acc;
            }
        }
        accepted_total.fetch_add(acc, std::memory_order_relaxed);
    };

    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t) ts.emplace_back(worker, t);
    for (auto& t : ts) t.join();

    int mismatches = 0;
    for (int64_t s = 0; s < NSYM; ++s) {
        int64_t expected = 0;
        for (int t = 0; t < THREADS; ++t) expected += local[t][s];
        if (gate.position(s) != expected) ++mismatches;
    }

    g_concurrency_ops = (long long)THREADS * OPS_PER_THREAD;
    std::printf("  %lld ops, %lld accepted, %d mismatches\n",
                g_concurrency_ops, accepted_total.load(), mismatches);
    expect(mismatches == 0, "stored position matches recount");
}

int main() {
    test_basic();
    test_boundaries();
    test_overflow();
    test_fail_closed();
    test_concurrency();

    std::printf("\n%d assertions, %d failed\n", g_checks, g_failed);
    if (g_failed == 0)
        std::printf("all tests passed (%d boundary cases, %lld concurrent ops)\n",
                    g_boundary_cases, g_concurrency_ops);
    return g_failed == 0 ? 0 : 1;
}
