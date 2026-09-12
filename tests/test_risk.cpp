#include "backtester/risk.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(double actual, double expected, double relative = 1e-10,
           double absolute = 1e-14) {
    return std::abs(actual - expected) <= absolute + relative * std::abs(expected);
}

void compare(const bt::RiskMetrics& actual, const bt::RiskMetrics& expected) {
    require(actual.observations == expected.observations, "risk observation count mismatch");
    require(close(actual.mean, expected.mean), "risk mean mismatch");
    require(close(actual.variance, expected.variance, 1e-9, 1e-28), "risk variance mismatch");
    require(close(actual.volatility, expected.volatility), "risk volatility mismatch");
    require(close(actual.sharpe, expected.sharpe, 1e-8), "risk Sharpe mismatch");
    require(close(actual.var95, expected.var95), "risk VaR mismatch");
}

} // namespace

void test_risk() {
    const std::array<double, 3> known{0.01, -0.02, 0.03};
    const auto metrics = bt::calculate_scalar(known.data(), known.size(), 100'000.0);
    const double mean = 0.02 / 3.0;
    const double variance = ((0.01 - mean) * (0.01 - mean) +
        (-0.02 - mean) * (-0.02 - mean) + (0.03 - mean) * (0.03 - mean)) / 2.0;
    require(close(metrics.mean, mean), "known return mean");
    require(close(metrics.variance, variance), "sample variance uses n-1");
    require(close(metrics.sharpe, mean / std::sqrt(variance)), "nonannualized Sharpe");
    require(close(metrics.var95, (1.6448536269514722 * std::sqrt(variance) - mean) * 100'000.0),
            "95% VaR convention");

    bt::RollingRisk rolling(100.0, bt::RiskMode::Scalar);
    require(rolling.calculate(100.0).observations == 0, "empty rolling history");
    rolling.observe(101.0);
    const auto insufficient = rolling.calculate(101.0);
    require(insufficient.observations == 1 && insufficient.sharpe == 0.0 &&
            insufficient.var95 == 0.0 && insufficient.variance == 0.0, "insufficient returns");
    rolling.observe(98.98);
    rolling.observe(101.9494);
    compare(rolling.calculate(100'000.0), metrics);

    bt::RollingRisk flat(100.0);
    for (int i = 0; i < 10; ++i) flat.observe(100.0);
    const auto flat_metrics = flat.calculate(100.0);
    require(flat_metrics.mean == 0.0 && flat_metrics.variance == 0.0 &&
            flat_metrics.sharpe == 0.0 && flat_metrics.var95 == 0.0, "flat equity risk");
    const std::array<double, 5> constant{-0.001, -0.001, -0.001, -0.001, -0.001};
    const auto constant_metrics = bt::calculate_scalar(constant.data(), constant.size(), 100.0);
    require(constant_metrics.variance == 0.0 && constant_metrics.sharpe == 0.0 &&
            close(constant_metrics.var95, 0.1), "constant negative returns retain expected loss VaR");
    require(bt::calculate_scalar(known.data(), known.size(), 0.0).var95 == 0.0,
            "zero equity VaR");
    require(bt::calculate_scalar(known.data(), known.size(), -1.0).var95 == 0.0,
            "negative equity VaR");
    require(bt::calculate_scalar(nullptr, 0, 100.0).observations == 0, "empty scalar input");

    bt::RollingRisk invalid(100.0, bt::RiskMode::Scalar);
    invalid.observe(0.0);
    invalid.observe(-1.0);
    invalid.observe(100.0);
    invalid.observe(101.0);
    invalid.observe(std::numeric_limits<double>::quiet_NaN());
    invalid.observe(100.0);
    invalid.observe(102.0);
    require(invalid.skipped_returns() == 5 && invalid.calculate(102.0).observations == 2,
            "invalid equity intervals skipped and rebased");
    const std::array<double, 2> valid{0.01, 0.02};
    compare(invalid.calculate(102.0), bt::calculate_scalar(valid.data(), valid.size(), 102.0));

    // Keep an independently ordered reference of the last window after wrapping.
    bt::RollingRisk wrapped(100.0, bt::RiskMode::Scalar);
    std::array<double, bt::risk_window_capacity> reference{};
    double equity = 100.0;
    constexpr std::size_t total = bt::risk_window_capacity + 37;
    for (std::size_t i = 0; i < total; ++i) {
        const double next = equity * (1.0 + (static_cast<int>(i % 13) - 6) * 0.0001);
        if (i >= total - reference.size()) {
            reference[i - (total - reference.size())] = (next - equity) / equity;
        }
        wrapped.observe(next);
        equity = next;
    }
    compare(wrapped.calculate(equity), bt::calculate_scalar(reference.data(), reference.size(), equity));

    if (!bt::avx2_available()) {
        require(!bt::RollingRisk(100.0).uses_avx2(), "automatic scalar fallback");
        compare(bt::calculate_avx2(known.data(), known.size(), 100'000.0), metrics);
        bool rejected = false;
        try { bt::RollingRisk unsupported(100.0, bt::RiskMode::Avx2); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "explicit unsupported AVX2 mode rejected");
        std::cout << "[SKIP] AVX2 equivalence: unavailable in this build or on this CPU/OS\n";
        return;
    }
    require(bt::RollingRisk(100.0).uses_avx2(), "automatic AVX2 selection");
    require(!bt::RollingRisk(100.0, bt::RiskMode::Scalar).uses_avx2(), "explicit scalar selection");
    bt::RollingRisk vector_rolling(100.0, bt::RiskMode::Avx2);
    vector_rolling.observe(101.0);
    vector_rolling.observe(98.98);
    vector_rolling.observe(101.9494);
    compare(vector_rolling.calculate(100'000.0), metrics);

    std::array<double, bt::risk_window_capacity + 4> values{};
    const std::array<std::size_t, 12> lengths{0, 1, 2, 3, 4, 5, 7, 8, 15, 31, 1023, 1024};
    for (int pattern = 0; pattern < 3; ++pattern) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            const double oscillation = static_cast<double>(static_cast<int>((i * 17) % 29) - 14);
            values[i] = pattern == 0 ? oscillation * 0.001 :
                        pattern == 1 ? 0.001 + oscillation * 1e-12 : 0.001;
        }
        for (std::size_t offset = 0; offset < 4; ++offset) {
            for (const auto length : lengths) {
                compare(bt::calculate_avx2(values.data() + offset, length, 123'456.0),
                        bt::calculate_scalar(values.data() + offset, length, 123'456.0));
            }
        }
    }
}
