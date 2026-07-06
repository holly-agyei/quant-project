// Latency and throughput benchmark for RiskGate, with a mutex + hash-map version
// as a point of comparison. Latency is timed per call with rdtscp on x86 (the
// TSC is calibrated against steady_clock at startup and the timer overhead is
// subtracted); elsewhere it falls back to steady_clock.
//
//   g++ -std=c++20 -O2 -pthread bench.cpp -o bench
#include "risk_gate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define HAVE_RDTSC 1
#else
#define HAVE_RDTSC 0
#endif

using namespace rg;
using Clock = std::chrono::steady_clock;

// Same three checks as RiskGate, but every call takes a mutex and looks the
// symbol up in a hash map instead of indexing an array.
class MutexRiskGate {
public:
    void configure(int64_t id, const SymbolLimits& lim) {
        std::lock_guard<std::mutex> g(m_);
        State& s = map_[id];
        s.limits = lim; s.net_position = 0; s.initialized = true;
    }
    Decision check(const Order& o) {
        std::lock_guard<std::mutex> g(m_);
        auto it = map_.find(o.symbol_id);
        if (it == map_.end() || !it->second.initialized)
            return Decision::RejectUninitialized;
        State& s = it->second;
        if (o.qty <= 0 || o.qty > s.limits.max_order_size)
            return Decision::RejectSize;
        if (o.price <= 0) return Decision::RejectPrice;
        int64_t band = 0;
        if (__builtin_mul_overflow(s.limits.reference_price,
                                   s.limits.price_band_bps, &band))
            return Decision::RejectOverflow;
        band /= BPS_DENOM;
        int64_t lo = 0, hi = 0;
        if (__builtin_sub_overflow(s.limits.reference_price, band, &lo) ||
            __builtin_add_overflow(s.limits.reference_price, band, &hi))
            return Decision::RejectOverflow;
        if (o.price < lo || o.price > hi) return Decision::RejectPrice;
        const int64_t signed_qty = (o.side == Side::Buy) ? o.qty : -o.qty;
        int64_t next = 0;
        if (__builtin_add_overflow(s.net_position, signed_qty, &next))
            return Decision::RejectOverflow;
        if (next > s.limits.position_limit || next < -s.limits.position_limit)
            return Decision::RejectPosition;
        s.net_position = next;
        return Decision::Accept;
    }
private:
    struct State { SymbolLimits limits{}; int64_t net_position{0}; bool initialized{false}; };
    std::mutex m_;
    std::unordered_map<int64_t, State> map_;
};

static inline uint64_t now_ticks() {
#if HAVE_RDTSC
    unsigned aux;
    return __rdtscp(&aux);
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch()).count();
#endif
}

static double g_ns_per_tick = 1.0;

static void calibrate() {
#if HAVE_RDTSC
    // Spin ~200ms and compare TSC ticks to elapsed wall-clock ns.
    auto c0 = Clock::now();
    uint64_t t0 = now_ticks();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - c0).count() < 200) { }
    uint64_t t1 = now_ticks();
    auto c1 = Clock::now();
    double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    c1 - c0).count();
    g_ns_per_tick = ns / (double)(t1 - t0);
#else
    g_ns_per_tick = 1.0;
#endif
}

static double pct(const std::vector<uint32_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    size_t idx = (size_t)(p * (sorted.size() - 1));
    return sorted[idx] * g_ns_per_tick;
}

// Smallest gap between two back-to-back timer reads, subtracted from each sample.
static uint64_t measure_timer_overhead() {
    uint64_t best = ~0ull;
    for (int i = 0; i < 200000; ++i) {
        uint64_t a = now_ticks();
        uint64_t b = now_ticks();
        if (b - a < best) best = b - a;
    }
    return best;
}

constexpr int64_t N = 12000;
constexpr long long WARMUP = 1'000'000;
constexpr long long MEASURE = 10'000'000;

// A stream of orders that all pass, spread across the whole universe, so the
// benchmark exercises the position commit rather than an early reject.
struct OrderStream {
    std::vector<Order> orders;
    explicit OrderStream(long long n) {
        std::mt19937_64 rng(12345);
        orders.reserve(n);
        for (long long i = 0; i < n; ++i) {
            int64_t sym = rng() % N;
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            orders.push_back(Order{sym, side, 100 * PRICE_SCALE, 1});
        }
    }
};

template <class Gate>
static double time_throughput(Gate& g, const std::vector<Order>& os, long long n) {
    volatile uint64_t sink = 0;
    auto c0 = Clock::now();
    for (long long i = 0; i < n; ++i)
        sink += (uint64_t)g.check(os[i % os.size()]);
    auto c1 = Clock::now();
    (void)sink;
    return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0)
        .count();
}

struct Lat { double p50, p99, p999, mx; };

template <class Gate>
static Lat time_latency(Gate& g, const std::vector<Order>& os, uint64_t overhead) {
    std::vector<uint32_t> samples;
    samples.reserve(MEASURE);
    volatile uint64_t sink = 0;
    for (long long i = 0; i < MEASURE; ++i) {
        const Order& o = os[i % os.size()];
        uint64_t a = now_ticks();
        sink += (uint64_t)g.check(o);
        uint64_t b = now_ticks();
        uint64_t d = (b - a > overhead) ? (b - a - overhead) : 0;
        samples.push_back((uint32_t)d);
    }
    (void)sink;
    std::sort(samples.begin(), samples.end());
    return Lat{pct(samples, 0.50), pct(samples, 0.99),
               pct(samples, 0.999), samples.back() * g_ns_per_tick};
}

// Several threads hit the shared gate across the whole universe. With a single
// mutex every symbol serialises; the lock-free gate lets separate symbols run
// in parallel. Returns aggregate throughput and fills `out` with merged latency.
template <class Gate>
static double time_mt(Gate& g, const std::vector<Order>& os, int threads,
                      long long per_thread, uint64_t overhead, Lat& out) {
    std::vector<std::vector<uint32_t>> samples(threads);
    auto worker = [&](int tid) {
        auto& s = samples[tid];
        s.reserve(per_thread);
        volatile uint64_t sink = 0;
        size_t base = (size_t)tid * 7919;
        for (long long i = 0; i < per_thread; ++i) {
            const Order& o = os[(base + i) % os.size()];
            uint64_t a = now_ticks();
            sink += (uint64_t)g.check(o);
            uint64_t b = now_ticks();
            uint64_t d = (b - a > overhead) ? (b - a - overhead) : 0;
            s.push_back((uint32_t)d);
        }
        (void)sink;
    };
    auto c0 = Clock::now();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) ts.emplace_back(worker, t);
    for (auto& t : ts) t.join();
    auto c1 = Clock::now();
    double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
                    c1 - c0).count();

    std::vector<uint32_t> all;
    for (auto& s : samples) all.insert(all.end(), s.begin(), s.end());
    std::sort(all.begin(), all.end());
    out = Lat{pct(all, 0.50), pct(all, 0.99), pct(all, 0.999),
              all.back() * g_ns_per_tick};
    return (double)(threads * per_thread) / (ns / 1e9);
}

int main() {
    calibrate();
#if HAVE_RDTSC
    std::printf("timer: rdtscp, %.4f ns/tick (%.2f GHz)\n",
                g_ns_per_tick, 1.0 / g_ns_per_tick);
#else
    std::printf("timer: steady_clock\n");
#endif
    uint64_t overhead = measure_timer_overhead();
    std::printf("timer overhead subtracted: %.2f ns\n\n",
                overhead * g_ns_per_tick);

    auto lockfree = std::make_unique<RiskGate<N>>();
    MutexRiskGate mutexed;
    for (int64_t i = 0; i < N; ++i) {
        SymbolLimits lim{INT64_MAX / 4, 100 * PRICE_SCALE, 1'000'000, 2000};
        lockfree->configure(i, lim);
        mutexed.configure(i, lim);
    }

    OrderStream stream(2'000'000);
    const auto& os = stream.orders;

    time_throughput(*lockfree, os, WARMUP);
    time_throughput(mutexed, os, WARMUP);

    Lat lf = time_latency(*lockfree, os, overhead);
    double lf_tps = MEASURE / (time_throughput(*lockfree, os, MEASURE) / 1e9);

    Lat mx = time_latency(mutexed, os, overhead);
    double mx_tps = MEASURE / (time_throughput(mutexed, os, MEASURE) / 1e9);

    unsigned hc = std::thread::hardware_concurrency();
    int threads = (int)std::min(8u, hc ? hc : 4u);
    long long per_thread = MEASURE / threads;
    for (int64_t i = 0; i < N; ++i)
        lockfree->configure(i, SymbolLimits{INT64_MAX / 4, 100 * PRICE_SCALE,
                                            1'000'000, 2000});
    Lat lf_mt, mx_mt;
    double lf_mt_tps = time_mt(*lockfree, os, threads, per_thread, overhead, lf_mt);
    double mx_mt_tps = time_mt(mutexed, os, threads, per_thread, overhead, mx_mt);

    const std::size_t bytes = RiskGate<N>::table_bytes();

    std::printf("single thread\n");
    std::printf("  lock-free   p50 %.0f ns   p99 %.0f ns   p99.9 %.0f ns   max %.0f ns   %.1f M/s\n",
                lf.p50, lf.p99, lf.p999, lf.mx, lf_tps / 1e6);
    std::printf("  mutex       p50 %.0f ns                                          %.1f M/s\n",
                mx.p50, mx_tps / 1e6);
    std::printf("\n%d threads\n", threads);
    std::printf("  lock-free   p50 %.0f ns   p99 %.0f ns   %.1f M/s\n",
                lf_mt.p50, lf_mt.p99, lf_mt_tps / 1e6);
    std::printf("  mutex       p50 %.0f ns   p99 %.0f ns   %.1f M/s\n",
                mx_mt.p50, mx_mt.p99, mx_mt_tps / 1e6);
    std::printf("\nthroughput ratio: %.1fx single, %.1fx at %d threads\n",
                lf_tps / mx_tps, lf_mt_tps / mx_mt_tps, threads);
    std::printf("symbol table: %lld symbols, %.1f KB\n",
                (long long)N, bytes / 1024.0);
    return 0;
}
