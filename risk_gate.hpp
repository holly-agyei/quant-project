// Pre-trade risk gate. Sits in front of order flow and vets every order against
// three limits: per-order size, a price band around a reference (fat-finger),
// and the net position cap for the symbol. Returns Accept or Reject.
//
// All math is int64 fixed-point so there's no float drift. Symbol state is a
// flat array indexed by id, and the position update is a CAS loop rather than a
// lock so orders on the same symbol can be checked concurrently. Anything it
// can't prove safe (bad id, unconfigured symbol, arithmetic overflow) rejects.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace rg {

// Prices are int64 in units of 1/PRICE_SCALE. With scale 10000, $100.50 stored
// as 1'005'000. Quantities are plain integer counts.
inline constexpr int64_t PRICE_SCALE = 10'000;
inline constexpr int64_t BPS_DENOM = 10'000; // 1000 bps = 10%

enum class Side : uint8_t { Buy, Sell };

// The reason field lets tests and monitoring see why an order was blocked; the
// fast path only cares whether it's Accept.
enum class Decision : uint8_t {
    Accept = 0,
    RejectBadSymbol,     // id out of range
    RejectUninitialized, // symbol never configured
    RejectSize,
    RejectPrice,
    RejectPosition,
    RejectOverflow,      // int64 math would wrap
};

constexpr bool accepted(Decision d) { return d == Decision::Accept; }

struct Order {
    int64_t symbol_id;
    Side    side;
    int64_t price; // fixed-point, scale = PRICE_SCALE
    int64_t qty;
};

struct SymbolLimits {
    int64_t position_limit;  // max |net position|
    int64_t reference_price; // fixed-point
    int64_t max_order_size;
    int64_t price_band_bps;  // half-width of the price band
};

template <int64_t N>
class RiskGate {
public:
    // Heap-allocate the table; entries start unconfigured and reject until set.
    RiskGate() : table_(std::make_unique<std::array<Entry, N>>()) {}

    static constexpr int64_t capacity() { return N; }
    static constexpr std::size_t table_bytes() { return sizeof(Entry) * N; }

    // Set up a symbol. Also resets its position to 0. Called at startup, not on
    // the hot path.
    void configure(int64_t id, const SymbolLimits& lim) {
        if (id < 0 || id >= N) return;
        Entry& e = (*table_)[id];
        e.position_limit = lim.position_limit;
        e.reference_price = lim.reference_price;
        e.max_order_size = lim.max_order_size;
        e.price_band_bps = lim.price_band_bps;
        e.net_position.store(0, std::memory_order_relaxed);
        e.initialized.store(true, std::memory_order_release);
    }

    int64_t position(int64_t id) const {
        if (id < 0 || id >= N) return 0;
        return (*table_)[id].net_position.load(std::memory_order_relaxed);
    }

    Decision check(const Order& o) {
        if (o.symbol_id < 0 || o.symbol_id >= N)
            return Decision::RejectBadSymbol;

        Entry& e = (*table_)[o.symbol_id];
        if (!e.initialized.load(std::memory_order_acquire))
            return Decision::RejectUninitialized;

        if (o.qty <= 0 || o.qty > e.max_order_size)
            return Decision::RejectSize;

        // Price must land inside reference +/- band. Guard the multiply so a
        // large reference or bps can't wrap into a bogus band.
        if (o.price <= 0)
            return Decision::RejectPrice;
        int64_t band = 0;
        if (__builtin_mul_overflow(e.reference_price, e.price_band_bps, &band))
            return Decision::RejectOverflow;
        band /= BPS_DENOM;
        int64_t lo = 0, hi = 0;
        if (__builtin_sub_overflow(e.reference_price, band, &lo) ||
            __builtin_add_overflow(e.reference_price, band, &hi))
            return Decision::RejectOverflow;
        if (o.price < lo || o.price > hi)
            return Decision::RejectPrice;

        const int64_t signed_qty = (o.side == Side::Buy) ? o.qty : -o.qty;

        // Read current position, work out the new one, check it, commit with a
        // CAS. If another order got in first, retry with the fresh value.
        int64_t cur = e.net_position.load(std::memory_order_relaxed);
        for (;;) {
            int64_t next = 0;
            if (__builtin_add_overflow(cur, signed_qty, &next))
                return Decision::RejectOverflow;

            // Compare both bounds to avoid negating INT64_MIN.
            if (next > e.position_limit || next < -e.position_limit)
                return Decision::RejectPosition;

            if (e.net_position.compare_exchange_weak(
                    cur, next,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed))
                return Decision::Accept;
        }
    }

private:
    struct Entry {
        std::atomic<int64_t> net_position{0};
        std::atomic<bool>    initialized{false};
        int64_t position_limit{0};
        int64_t reference_price{0};
        int64_t max_order_size{0};
        int64_t price_band_bps{0};
    };

    std::unique_ptr<std::array<Entry, N>> table_;
};

} // namespace rg
