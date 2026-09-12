#pragma once

#include <array>
#include <cstddef>

namespace bt {

inline constexpr std::size_t risk_window_capacity = 1024;

enum class RiskMode { Auto, Scalar, Avx2 };

struct RiskMetrics {
    std::size_t observations{};
    double mean{};
    double variance{};
    double volatility{};
    double sharpe{};
    double var95{};
};

bool avx2_available() noexcept;

// Input is a contiguous sequence of finite returns. Fewer than two samples
// produces zero-valued statistics; no annualization or risk-free adjustment.
RiskMetrics calculate_scalar(const double* returns, std::size_t count,
                             double equity) noexcept;
// Safe on every supported CPU: falls back to scalar if AVX2 is unavailable.
RiskMetrics calculate_avx2(const double* returns, std::size_t count,
                           double equity) noexcept;

class RollingRisk {
public:
    // An explicitly requested, unavailable AVX2 mode throws at initialization.
    explicit RollingRisk(double initial_equity, RiskMode mode = RiskMode::Auto);

    void observe(double equity) noexcept;
    RiskMetrics calculate(double equity) const noexcept;
    bool uses_avx2() const noexcept { return uses_avx2_; }
    std::size_t skipped_returns() const noexcept { return skipped_returns_; }

private:
    std::array<double, risk_window_capacity> returns_{};
    std::size_t next_{};
    std::size_t count_{};
    std::size_t skipped_returns_{};
    double previous_equity_{};
    bool uses_avx2_{};
};

namespace detail {
// Shared conversion from two-pass moments to reported risk statistics.
RiskMetrics finish_risk(std::size_t count, double mean, double squared_deviations,
                        double equity) noexcept;
}

} // namespace bt
