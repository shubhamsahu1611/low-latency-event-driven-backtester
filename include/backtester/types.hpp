#pragma once

#include <cstdint>
#include <type_traits>

namespace bt {

// Supported quote/mark domain bounds arithmetic in this simplified simulator.
inline constexpr double max_market_price = 1e9;

struct Tick {
    std::uint64_t timestamp_ns{};
    double bid{};
    double ask{};
    double last{};
    double liquidity{}; // Reference liquidity, in shares; not a hard fill limit.
    std::uint32_t instrument{};
};
static_assert(sizeof(Tick) <= 48);
static_assert(std::is_trivially_copyable_v<Tick>);

enum class Side { Buy, Sell };

struct Order {
    Side side{};
    std::uint32_t quantity{};
};

struct Fill {
    Side side{};
    std::uint32_t quantity{};
    double price{};
};

} // namespace bt
