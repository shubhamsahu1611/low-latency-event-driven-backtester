#include "backtester/risk.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace bt {

#if defined(BT_HAS_AVX2) && BT_HAS_AVX2
// Defined in the only translation unit compiled with AVX2 enabled.
RiskMetrics calculate_avx2_impl(const double*, std::size_t, double) noexcept;
#endif

bool avx2_available() noexcept {
    static const bool supported = []() noexcept {
#if !defined(BT_HAS_AVX2) || !BT_HAS_AVX2
        return false;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        int registers[4]{};
        __cpuid(registers, 0);
        if (registers[0] < 7) return false;
        __cpuidex(registers, 1, 0);
        constexpr int osxsave = 1 << 27;
        constexpr int avx = 1 << 28;
        if ((registers[2] & (osxsave | avx)) != (osxsave | avx)) return false;
        // The OS must save both the XMM and YMM register state.
        if ((_xgetbv(0) & 0x6) != 0x6) return false;
        __cpuidex(registers, 7, 0);
        return (registers[1] & (1 << 5)) != 0;
#elif (defined(__GNUC__) || defined(__clang__)) && \
      (defined(__x86_64__) || defined(__i386__))
        // The compiler's probe includes OS support for saving AVX state.
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") != 0;
#else
        return false;
#endif
    }();
    return supported;
}

RiskMetrics detail::finish_risk(std::size_t count, double mean,
                                double squared_deviations, double equity) noexcept {
    RiskMetrics result{};
    result.observations = count;
    if (count < 2) return result;
    result.mean = mean;
    result.variance = std::max(0.0, squared_deviations / static_cast<double>(count - 1));
    result.volatility = std::sqrt(result.variance);
    result.sharpe = result.volatility > 0.0 ? mean / result.volatility : 0.0;
    constexpr double normal_95 = 1.6448536269514722;
    const double positive_equity = std::isfinite(equity) && equity > 0.0 ? equity : 0.0;
    result.var95 = std::max(0.0, normal_95 * result.volatility - mean) * positive_equity;
    return result;
}

RiskMetrics calculate_scalar(const double* returns, std::size_t count,
                             double equity) noexcept {
    if (count < 2) return detail::finish_risk(count, 0.0, 0.0, equity);
    // Centering the first pass preserves precision for nearly constant returns.
    const double anchor = returns[0];
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) sum += returns[i] - anchor;
    const double mean = anchor + sum / static_cast<double>(count);
    double squared_deviations = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double difference = returns[i] - mean;
        squared_deviations += difference * difference;
    }
    return detail::finish_risk(count, mean, squared_deviations, equity);
}

RiskMetrics calculate_avx2(const double* returns, std::size_t count,
                           double equity) noexcept {
#if defined(BT_HAS_AVX2) && BT_HAS_AVX2
    if (avx2_available()) return calculate_avx2_impl(returns, count, equity);
#endif
    return calculate_scalar(returns, count, equity);
}

RollingRisk::RollingRisk(double initial_equity, RiskMode mode)
    : previous_equity_(initial_equity),
      uses_avx2_(mode != RiskMode::Scalar && avx2_available()) {
    if (mode == RiskMode::Avx2 && !uses_avx2_) {
        throw std::runtime_error("AVX2 risk requested but unavailable in this build or on this CPU/OS");
    }
}

void RollingRisk::observe(double equity) noexcept {
    // Returns crossing zero or non-finite equity are undefined. Rebase so the
    // next valid positive-to-positive interval can resume the rolling series.
    const double previous = previous_equity_;
    previous_equity_ = equity;
    if (!(previous > 0.0) || !(equity > 0.0) ||
        !std::isfinite(previous) || !std::isfinite(equity)) {
        ++skipped_returns_;
        return;
    }
    const double value = (equity - previous) / previous;
    if (!std::isfinite(value)) {
        ++skipped_returns_;
        return;
    }
    returns_[next_] = value;
    next_ = (next_ + 1) % risk_window_capacity;
    if (count_ < risk_window_capacity) ++count_;
}

RiskMetrics RollingRisk::calculate(double equity) const noexcept {
    // Moments do not depend on chronological order, so wrapping needs no copy.
    if (uses_avx2_) return calculate_avx2(returns_.data(), count_, equity);
    return calculate_scalar(returns_.data(), count_, equity);
}

} // namespace bt
