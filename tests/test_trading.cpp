#include "backtester/engine.hpp"
#include "backtester/trading.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void close(double actual, double expected, const char* message,
           double absolute = 1e-9, double relative = 1e-11) {
    require(std::isfinite(actual) && std::isfinite(expected) &&
                std::abs(actual - expected) <= absolute + relative * std::abs(expected),
            message);
}

template <typename Function>
void invalid(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, message);
}

bt::Tick tick(double price, std::uint64_t timestamp = 0, double liquidity = 1'000.0) {
    return {timestamp, price - 0.01, price + 0.01, price, liquidity, 0};
}

void test_execution() {
    const auto market = tick(100.0);
    bt::Fill buy{}, sell{};
    const bt::ExecutionModel free_execution({0.0, 0.0});
    require(free_execution.execute({bt::Side::Buy, 100}, market, buy), "buy must fill");
    require(free_execution.execute({bt::Side::Sell, 100}, market, sell), "sell must fill");
    close(buy.price, market.ask, "buy must cross to the ask");
    close(sell.price, market.bid, "sell must cross to the bid");
    require(buy.side == bt::Side::Buy && sell.side == bt::Side::Sell &&
                buy.quantity == 100 && sell.quantity == 100,
            "execution must preserve order side and full quantity");

    const bt::ExecutionModel slippage({2.0, 0.0});
    require(slippage.execute({bt::Side::Buy, 100}, market, buy), "slipped buy must fill");
    require(slippage.execute({bt::Side::Sell, 100}, market, sell), "slipped sell must fill");
    close(buy.price, market.ask * 1.0002, "buy slippage must increase execution price");
    close(sell.price, market.bid * 0.9998, "sell slippage must decrease execution price");

    const bt::ExecutionModel impact({0.0, 20.0});
    bt::Fill larger{}, thinner{};
    require(impact.execute({bt::Side::Buy, 100}, market, buy), "small impacted buy must fill");
    require(impact.execute({bt::Side::Buy, 200}, market, larger), "larger impacted buy must fill");
    require(impact.execute({bt::Side::Buy, 100}, tick(100.0, 0, 500.0), thinner),
            "lower-liquidity buy must fill");
    const double small_cost = buy.price / market.ask - 1.0;
    const double large_cost = larger.price / market.ask - 1.0;
    require(small_cost > 0.0 && large_cost > 2.0 * small_cost,
            "larger participation must increase impact disproportionately");
    close(large_cost, 4.0 * small_cost, "doubling quantity must quadruple quadratic impact", 1e-14);
    close(thinner.price, larger.price, "halving liquidity must equal doubling quantity");
    require(impact.execute({bt::Side::Sell, 200}, market, sell), "impacted sell must fill");
    close(sell.price, market.bid * (1.0 - large_cost), "sell impact must be adverse");
    require(impact.execute({bt::Side::Buy, 2'000}, market, buy) && buy.quantity == 2'000,
            "liquidity is a cost reference, not a partial-fill cap");

    require(!impact.execute({bt::Side::Buy, 0}, market, buy), "zero-sized orders must be rejected");
    for (double liquidity : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        require(!impact.execute({bt::Side::Buy, 10}, tick(100.0, 0, liquidity), buy),
                "nonpositive or nonfinite liquidity must be rejected");
    }
    const bt::ExecutionModel excessive_cost({10'001.0, 0.0});
    require(!excessive_cost.execute({bt::Side::Sell, 100}, market, sell),
            "costs producing a nonpositive sell price must be rejected");
    auto invalid_quote = market;
    invalid_quote.ask = std::numeric_limits<double>::infinity();
    require(!impact.execute({bt::Side::Buy, 100}, invalid_quote, buy),
            "nonfinite execution price must be rejected");
    auto overflowing_notional = market;
    overflowing_notional.ask = 1e9;
    overflowing_notional.liquidity = 5e-151;
    require(!bt::ExecutionModel().execute({bt::Side::Buy, 10}, overflowing_notional, buy),
            "finite prices with overflowing fill notionals must be rejected");

    for (double cost : {-1.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        invalid([&] { bt::ExecutionModel model({cost, 0.0}); }, "invalid slippage must throw");
        invalid([&] { bt::ExecutionModel model({0.0, cost}); }, "invalid impact must throw");
    }
}

void check_portfolio(const bt::Portfolio& portfolio, std::int64_t position,
                     double cash, double average, double realized, double mark,
                     double unrealized) {
    require(portfolio.position() == position, "portfolio position mismatch");
    close(portfolio.cash(), cash, "portfolio cash mismatch");
    close(portfolio.average_price(), average, "portfolio cost basis mismatch");
    close(portfolio.realized_pnl(), realized, "realized PnL mismatch");
    close(portfolio.unrealized_pnl(mark), unrealized, "unrealized PnL mismatch");
    close(portfolio.equity(mark), cash + static_cast<double>(position) * mark,
          "equity must equal cash plus marked position");
    close(portfolio.pnl(mark), realized + unrealized, "realized plus unrealized must equal total PnL");
}

void test_portfolio() {
    bt::Portfolio portfolio(10'000.0);
    portfolio.apply({bt::Side::Buy, 10, 100.0});
    check_portfolio(portfolio, 10, 9'000.0, 100.0, 0.0, 110.0, 100.0);
    portfolio.apply({bt::Side::Buy, 10, 120.0});
    check_portfolio(portfolio, 20, 7'800.0, 110.0, 0.0, 115.0, 100.0);
    portfolio.apply({bt::Side::Sell, 5, 130.0});
    check_portfolio(portfolio, 15, 8'450.0, 110.0, 100.0, 115.0, 75.0);
    portfolio.apply({bt::Side::Sell, 15, 90.0});
    check_portfolio(portfolio, 0, 9'800.0, 0.0, -200.0, 90.0, 0.0);
    portfolio.apply({bt::Side::Sell, 8, 100.0});
    check_portfolio(portfolio, -8, 10'600.0, 100.0, -200.0, 90.0, 80.0);
    portfolio.apply({bt::Side::Buy, 3, 80.0});
    check_portfolio(portfolio, -5, 10'360.0, 100.0, -140.0, 90.0, 50.0);
    portfolio.apply({bt::Side::Buy, 9, 110.0});
    check_portfolio(portfolio, 4, 9'370.0, 110.0, -190.0, 115.0, 20.0);
    portfolio.apply({bt::Side::Sell, 7, 120.0});
    check_portfolio(portfolio, -3, 10'210.0, 120.0, -150.0, 115.0, 15.0);
    portfolio.apply({bt::Side::Buy, 3, 115.0});
    check_portfolio(portfolio, 0, 9'865.0, 0.0, -135.0, 115.0, 0.0);

    for (double cash : {0.0, -1.0, std::numeric_limits<double>::infinity()})
        invalid([&] { bt::Portfolio invalid_portfolio(cash); }, "invalid initial cash must throw");
}

void test_strategy() {
    bt::MomentumStrategy strategy({2, 10, 0.05});
    bt::Order order{};
    require(!strategy.on_tick(tick(100.0), 0, order), "strategy must wait for its interval");
    require(strategy.on_tick(tick(100.2), 0, order) && order.side == bt::Side::Buy && order.quantity == 10,
            "rising price must target a bounded long position");
    require(!strategy.on_tick(tick(100.3), 10, order), "strategy must observe the next interval");
    require(!strategy.on_tick(tick(100.5), 10, order), "strategy must not add beyond its long target");
    require(!strategy.on_tick(tick(100.4), 10, order), "strategy must wait before reversal");
    require(strategy.on_tick(tick(100.2), 10, order) && order.side == bt::Side::Sell && order.quantity == 20,
            "falling price must reverse to the short target");
    require(!strategy.on_tick(tick(100.2), -10, order), "strategy must wait after reversal");
    require(!strategy.on_tick(tick(100.21), -10, order), "subthreshold change must preserve position");

    std::int64_t position = -10;
    std::uint64_t orders = 0;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        if (strategy.on_tick(tick((i / 2) % 2 == 0 ? 101.0 : 100.0), position, order)) {
            require(order.quantity > 0 && order.quantity <= 20, "strategy order size must remain bounded");
            position += order.side == bt::Side::Buy ? order.quantity : -std::int64_t(order.quantity);
            ++orders;
        }
        require(std::abs(position) <= 10, "strategy position must remain bounded");
    }
    require(orders > 10, "alternating prices must exercise repeated strategy orders");
    invalid([] { bt::MomentumStrategy bad({0, 10, 0.01}); }, "zero strategy interval must throw");
    invalid([] { bt::MomentumStrategy bad({1, 0, 0.01}); }, "zero strategy size must throw");
    invalid([] { bt::MomentumStrategy bad({1, 10, -0.01}); }, "negative strategy threshold must throw");
}

void compare_metrics(const bt::RiskMetrics& actual, const bt::RiskMetrics& expected) {
    require(actual.observations == expected.observations, "engine risk sample counts must match");
    close(actual.mean, expected.mean, "scalar/AVX2 engine risk means differ", 1e-18);
    close(actual.variance, expected.variance, "scalar/AVX2 engine risk variances differ", 1e-22);
    close(actual.volatility, expected.volatility, "scalar/AVX2 engine volatilities differ", 1e-18);
    close(actual.sharpe, expected.sharpe, "scalar/AVX2 engine Sharpes differ", 1e-12);
    close(actual.var95, expected.var95, "scalar/AVX2 engine VaRs differ", 1e-12);
}

void test_engine() {
    bt::EngineConfig config;
    config.strategy = {16, 7, 0.005};
    config.risk_interval = 257;
    config.risk_mode = bt::RiskMode::Scalar;
    bt::BacktestEngine scalar(config);
    scalar.finish();
    require(scalar.risk_updates() == 0, "empty engine finish must not invent risk observations");
    std::optional<bt::BacktestEngine> avx;
    if (bt::avx2_available()) {
        config.risk_mode = bt::RiskMode::Avx2;
        avx.emplace(config);
        require(avx->uses_avx2(), "explicit AVX2 engine must use AVX2 when supported");
    }
    constexpr std::uint64_t count = 100'000;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto phase = i % 2'000;
        const double price = 100.0 + 0.001 * static_cast<double>(phase <= 1'000 ? phase : 2'000 - phase);
        const auto market = tick(price, i * 1'000'000);
        scalar.on_tick(market);
        if (avx) avx->on_tick(market);
        require(std::abs(scalar.portfolio().position()) <= 7, "engine strategy must respect position bounds");
    }
    require(scalar.risk_updates() == count / config.risk_interval,
            "engine risk must refresh at the configured event interval");
    scalar.finish();
    const auto updates = (count + config.risk_interval - 1) / config.risk_interval;
    require(scalar.risk_updates() == updates, "finish must include one final incomplete interval");
    scalar.finish();
    require(scalar.risk_updates() == updates, "finish must be idempotent");
    require(scalar.ticks() == count && scalar.orders() > 0 && scalar.orders() == scalar.fills() &&
                scalar.rejected_orders() == 0, "engine must process ticks through orders and fills");
    require(scalar.skipped_returns() == 0 && scalar.metrics().observations == bt::risk_window_capacity,
            "engine risk must use the rolling equity return window");
    require(!scalar.uses_avx2(), "explicit scalar mode must remain scalar");
    const auto& portfolio = scalar.portfolio();
    close(portfolio.pnl(scalar.mark()), portfolio.realized_pnl() + portfolio.unrealized_pnl(scalar.mark()),
          "full engine must preserve portfolio accounting identity", 1e-7);
    require(std::isfinite(scalar.metrics().sharpe) && std::isfinite(scalar.metrics().var95) &&
                scalar.metrics().var95 >= 0.0, "full engine must report finite risk metrics");
    if (avx) {
        avx->finish();
        require(avx->ticks() == scalar.ticks() && avx->orders() == scalar.orders() &&
                    avx->fills() == scalar.fills() && avx->risk_updates() == scalar.risk_updates(),
                "risk implementation choice must preserve engine event counts");
        close(avx->portfolio().equity(avx->mark()), portfolio.equity(scalar.mark()),
              "risk implementation choice must preserve portfolio equity");
        compare_metrics(avx->metrics(), scalar.metrics());
    }

    config.risk_mode = bt::RiskMode::Scalar;
    config.risk_interval = 4;
    bt::BacktestEngine exact_interval(config);
    for (std::uint64_t i = 0; i < 8; ++i) exact_interval.on_tick(tick(100.0, i));
    exact_interval.finish();
    require(exact_interval.risk_updates() == 2, "finish at an exact interval must not duplicate risk work");
    config.risk_interval = 0;
    invalid([&] { bt::BacktestEngine bad(config); }, "zero risk interval must throw");
}

} // namespace

void test_trading() {
    test_execution();
    test_portfolio();
    test_strategy();
    test_engine();
}
