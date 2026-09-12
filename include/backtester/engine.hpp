#pragma once

#include "backtester/object_pool.hpp"
#include "backtester/risk.hpp"
#include "backtester/trading.hpp"

#include <cstdint>
#include <stdexcept>

namespace bt {

struct EngineConfig {
    double initial_cash = 1'000'000.0;
    StrategyConfig strategy{};
    ExecutionConfig execution{};
    std::uint32_t risk_interval = 256;
    RiskMode risk_mode = RiskMode::Auto;
};

class BacktestEngine {
public:
    explicit BacktestEngine(EngineConfig config = {})
        : strategy_(config.strategy), execution_(config.execution),
          portfolio_(config.initial_cash), risk_(config.initial_cash, config.risk_mode),
          risk_interval_(config.risk_interval) {
        if (risk_interval_ == 0) throw std::invalid_argument("Risk interval must be positive");
    }

    // Input contract: one instrument (id 0), finite positive prices/liquidity,
    // prices <= max_market_price, bid <= ask, and nondecreasing timestamps.
    // Sources validate before replay.
    void on_tick(const Tick& tick) noexcept {
        ++ticks_;
        mark_ = tick.last;
        Order candidate;
        if (strategy_.on_tick(tick, portfolio_.position(), candidate)) {
            ++orders_;
            if (Order* order = orders_pool_.acquire()) {
                *order = candidate;
                Fill fill;
                if (execution_.execute(*order, tick, fill)) {
                    portfolio_.apply(fill);
                    ++fills_;
                } else ++rejected_orders_;
                orders_pool_.release(order);
            } else ++rejected_orders_;
        }
        risk_.observe(portfolio_.equity(mark_));
        if (++since_risk_ == risk_interval_) {
            refresh_risk();
            since_risk_ = 0;
        }
    }

    // Final snapshot is included in the benchmark timer, even for a short run.
    void finish() noexcept { if (since_risk_ != 0) { refresh_risk(); since_risk_ = 0; } }
    const Portfolio& portfolio() const noexcept { return portfolio_; }
    const RiskMetrics& metrics() const noexcept { return metrics_; }
    std::uint64_t ticks() const noexcept { return ticks_; }
    std::uint64_t orders() const noexcept { return orders_; }
    std::uint64_t fills() const noexcept { return fills_; }
    std::uint64_t rejected_orders() const noexcept { return rejected_orders_; }
    std::uint64_t risk_updates() const noexcept { return risk_updates_; }
    std::size_t skipped_returns() const noexcept { return risk_.skipped_returns(); }
    bool uses_avx2() const noexcept { return risk_.uses_avx2(); }
    double mark() const noexcept { return mark_; }

private:
    void refresh_risk() noexcept { metrics_ = risk_.calculate(portfolio_.equity(mark_)); ++risk_updates_; }
    MomentumStrategy strategy_;
    ExecutionModel execution_;
    Portfolio portfolio_;
    RollingRisk risk_;
    ObjectPool<Order, 16> orders_pool_; // Consumer owns it; no atomics are needed.
    RiskMetrics metrics_{};
    std::uint64_t ticks_{}, orders_{}, fills_{}, rejected_orders_{}, risk_updates_{};
    std::uint32_t risk_interval_, since_risk_{};
    double mark_{};
};

} // namespace bt
