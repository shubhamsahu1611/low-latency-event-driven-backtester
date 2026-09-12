#pragma once

#include "backtester/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace bt {

struct ExecutionConfig {
    double slippage_bps = 0.5;
    double impact_bps = 2.0;
};

class ExecutionModel {
public:
    explicit ExecutionModel(ExecutionConfig config = {}) : config_(config) {
        if (!std::isfinite(config.slippage_bps) || config.slippage_bps < 0.0 ||
            !std::isfinite(config.impact_bps) || config.impact_bps < 0.0) {
            throw std::invalid_argument("Execution costs must be finite and nonnegative");
        }
    }

    // All-or-none immediate execution. Excess participation raises cost instead
    // of generating partial fills. Quadratic impact is deliberately convex.
    bool execute(const Order& order, const Tick& tick, Fill& fill) const noexcept {
        if (order.quantity == 0 || !(tick.liquidity > 0.0) ||
            !std::isfinite(tick.liquidity)) return false;
        const double participation = order.quantity / tick.liquidity;
        const double adverse = (config_.slippage_bps +
            config_.impact_bps * participation * participation) * 1e-4;
        const double quote = order.side == Side::Buy ? tick.ask : tick.bid;
        const double price = quote * (order.side == Side::Buy ? 1.0 + adverse : 1.0 - adverse);
        if (!(price > 0.0) || !std::isfinite(price) ||
            !std::isfinite(price * order.quantity)) return false;
        fill = {order.side, order.quantity, price};
        return true;
    }

private:
    ExecutionConfig config_;
};

class Portfolio {
public:
    explicit Portfolio(double initial_cash = 1'000'000.0)
        : initial_cash_(initial_cash), cash_(initial_cash) {
        if (!(initial_cash > 0.0) || !std::isfinite(initial_cash))
            throw std::invalid_argument("Initial cash must be finite and positive");
    }

    void apply(const Fill& fill) noexcept {
        const std::int64_t delta = fill.side == Side::Buy ? fill.quantity : -std::int64_t(fill.quantity);
        const auto old_size = std::abs(position_);
        const auto fill_size = std::int64_t(fill.quantity);
        cash_ -= static_cast<double>(delta) * fill.price;
        if (position_ == 0 || (position_ > 0) == (delta > 0)) {
            average_price_ = (average_price_ * static_cast<double>(old_size) +
                fill.price * static_cast<double>(fill_size)) / static_cast<double>(old_size + fill_size);
        } else {
            const auto closing = std::min(old_size, fill_size);
            realized_pnl_ += static_cast<double>(closing) * (fill.price - average_price_) *
                (position_ > 0 ? 1.0 : -1.0);
            if (fill_size > old_size) average_price_ = fill.price; // Reversal opens at fill price.
            else if (fill_size == old_size) average_price_ = 0.0;
        }
        position_ += delta;
    }

    std::int64_t position() const noexcept { return position_; }
    double cash() const noexcept { return cash_; }
    double average_price() const noexcept { return average_price_; }
    double realized_pnl() const noexcept { return realized_pnl_; }
    double unrealized_pnl(double mark) const noexcept {
        return static_cast<double>(position_) * (mark - average_price_);
    }
    double equity(double mark) const noexcept { return cash_ + static_cast<double>(position_) * mark; }
    double pnl(double mark) const noexcept { return equity(mark) - initial_cash_; }

private:
    double initial_cash_;
    double cash_;
    std::int64_t position_{};
    double average_price_{};
    double realized_pnl_{};
};

struct StrategyConfig {
    std::uint32_t interval = 64;
    std::uint32_t size = 10;
    double threshold = 0.01; // Price units between decision ticks.
};

class MomentumStrategy {
public:
    explicit MomentumStrategy(StrategyConfig config = {}) : config_(config) {
        if (config.interval == 0 || config.size == 0 || config.size > 1'000'000 ||
            !std::isfinite(config.threshold) || config.threshold < 0.0)
            throw std::invalid_argument("Invalid strategy configuration");
    }

    bool on_tick(const Tick& tick, std::int64_t position, Order& order) noexcept {
        if (!initialized_) { reference_ = tick.last; initialized_ = true; }
        if (++since_decision_ < config_.interval) return false;
        since_decision_ = 0;
        const double change = tick.last - reference_;
        reference_ = tick.last;
        const std::int64_t target = change > config_.threshold ? config_.size :
            (change < -config_.threshold ? -std::int64_t(config_.size) : position);
        const auto delta = target - position;
        if (delta == 0) return false;
        order = {delta > 0 ? Side::Buy : Side::Sell, static_cast<std::uint32_t>(std::abs(delta))};
        return true;
    }

private:
    StrategyConfig config_;
    std::uint32_t since_decision_{};
    double reference_{};
    bool initialized_{};
};

} // namespace bt
