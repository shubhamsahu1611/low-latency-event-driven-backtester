#pragma once

#include "backtester/types.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace bt {

// A bounded, deterministic stress feed. This is not a predictive market model.
class SyntheticSource {
public:
    Tick next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        const double mid = 100.0 +
            (static_cast<double>((state_ >> 8) & 1023U) - 512.0) * 0.0001;
        timestamp_ns_ += 1'000;
        return {timestamp_ns_, mid - 0.01, mid + 0.01, mid,
                1'000.0 + static_cast<double>(state_ & 255U), 0};
    }

private:
    std::uint32_t state_{0x12345678U};
    std::uint64_t timestamp_ns_{};
};

// CSV loading and validation happen before timed event processing.
std::vector<Tick> parse_csv(std::istream& input);
std::vector<Tick> load_csv(const std::string& path);

} // namespace bt
