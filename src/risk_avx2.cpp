#include "backtester/risk.hpp"

#include <immintrin.h>

namespace bt {
namespace {

double horizontal_sum(__m256d value) noexcept {
    // VPERMPD is an AVX2 instruction: swap 128-bit halves, then add adjacent
    // lanes. All four lane contributions feed the reported risk statistics.
    const __m256d pairs = _mm256_add_pd(
        value, _mm256_permute4x64_pd(value, _MM_SHUFFLE(1, 0, 3, 2)));
    const __m128d low = _mm256_castpd256_pd128(pairs);
    return _mm_cvtsd_f64(_mm_add_sd(low, _mm_unpackhi_pd(low, low)));
}

} // namespace

RiskMetrics calculate_avx2_impl(const double* returns, std::size_t count,
                                double equity) noexcept {
    if (count < 2) return detail::finish_risk(count, 0.0, 0.0, equity);
    const double anchor = returns[0];
    const __m256d anchor_vector = _mm256_set1_pd(anchor);
    __m256d sum_vector = _mm256_setzero_pd();
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        sum_vector = _mm256_add_pd(sum_vector,
            _mm256_sub_pd(_mm256_loadu_pd(returns + i), anchor_vector));
    }
    double sum = horizontal_sum(sum_vector);
    for (; i < count; ++i) sum += returns[i] - anchor;
    const double mean = anchor + sum / static_cast<double>(count);

    const __m256d mean_vector = _mm256_set1_pd(mean);
    __m256d squares_vector = _mm256_setzero_pd();
    i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256d difference = _mm256_sub_pd(_mm256_loadu_pd(returns + i), mean_vector);
        squares_vector = _mm256_add_pd(squares_vector, _mm256_mul_pd(difference, difference));
    }
    double squared_deviations = horizontal_sum(squares_vector);
    for (; i < count; ++i) {
        const double difference = returns[i] - mean;
        squared_deviations += difference * difference;
    }
    return detail::finish_risk(count, mean, squared_deviations, equity);
}

} // namespace bt
