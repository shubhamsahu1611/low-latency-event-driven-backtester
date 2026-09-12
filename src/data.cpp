#include "backtester/data.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <string_view>

namespace bt {
namespace {

[[noreturn]] void fail(std::size_t line, const std::string& reason) {
    throw std::runtime_error("CSV line " + std::to_string(line) + ": " + reason);
}

template <typename T>
T number(std::string_view field, std::size_t line, const char* name) {
    T value{};
    const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
    if (field.empty() || result.ec != std::errc{} ||
        result.ptr != field.data() + field.size()) {
        fail(line, std::string("invalid ") + name);
    }
    return value;
}

void remove_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

} // namespace

std::vector<Tick> parse_csv(std::istream& input) {
    std::string line;
    if (!std::getline(input, line)) {
        fail(1, "missing header");
    }
    remove_cr(line);
    if (line != "timestamp_ns,instrument,bid,ask,last,liquidity") {
        fail(1, "expected header timestamp_ns,instrument,bid,ask,last,liquidity");
    }

    std::vector<Tick> ticks;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        remove_cr(line);
        if (line.empty()) {
            continue;
        }

        std::array<std::string_view, 6> fields;
        std::string_view remainder(line);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto comma = remainder.find(',');
            const bool last_field = i == fields.size() - 1;
            if ((!last_field && comma == std::string_view::npos) ||
                (last_field && comma != std::string_view::npos)) {
                fail(line_number, "expected exactly six fields");
            }
            fields[i] = remainder.substr(0, comma);
            if (comma != std::string_view::npos) {
                remainder.remove_prefix(comma + 1);
            }
        }

        Tick tick{};
        tick.timestamp_ns = number<std::uint64_t>(fields[0], line_number, "timestamp_ns");
        tick.instrument = number<std::uint32_t>(fields[1], line_number, "instrument");
        tick.bid = number<double>(fields[2], line_number, "bid");
        tick.ask = number<double>(fields[3], line_number, "ask");
        tick.last = number<double>(fields[4], line_number, "last");
        tick.liquidity = number<double>(fields[5], line_number, "liquidity");
        if (tick.instrument != 0) {
            fail(line_number, "only instrument 0 is supported");
        }
        if (!ticks.empty() && tick.timestamp_ns < ticks.back().timestamp_ns) {
            fail(line_number, "timestamps must be nondecreasing");
        }
        if (!std::isfinite(tick.bid) || !std::isfinite(tick.ask) ||
            !std::isfinite(tick.last) || !std::isfinite(tick.liquidity) ||
            tick.bid <= 0 || tick.ask <= 0 || tick.last <= 0 || tick.liquidity <= 0) {
            fail(line_number, "prices and liquidity must be finite and positive");
        }
        if (tick.bid > max_market_price || tick.ask > max_market_price ||
            tick.last > max_market_price) {
            fail(line_number, "prices must not exceed the supported maximum of 1e9");
        }
        if (tick.bid > tick.ask) {
            fail(line_number, "bid must not exceed ask");
        }
        ticks.push_back(tick);
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        fail(line_number, "input read failed");
    }
    if (ticks.empty()) {
        fail(line_number, "no market ticks");
    }
    return ticks;
}

std::vector<Tick> load_csv(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open CSV: " + path);
    }
    return parse_csv(input);
}

} // namespace bt
