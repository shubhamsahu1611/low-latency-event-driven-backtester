#include "backtester/data.hpp"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

constexpr const char* header = "timestamp_ns,instrument,bid,ask,last,liquidity\n";

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void reject(const std::string& csv, const char* expected) {
    std::istringstream input(csv);
    try {
        static_cast<void>(bt::parse_csv(input));
    } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
                "CSV error should explain the invalid input");
        return;
    }
    throw std::runtime_error("CSV parser accepted invalid input");
}

} // namespace

void test_data() {
    bt::SyntheticSource first;
    bt::SyntheticSource second;
    double initial_mid = 0;
    bool moved = false;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        const auto a = first.next();
        const auto b = second.next();
        require(a.timestamp_ns == (i + 1) * 1'000 && a.instrument == 0,
                "synthetic event metadata is invalid");
        require(a.timestamp_ns == b.timestamp_ns && a.instrument == b.instrument &&
                a.bid == b.bid && a.ask == b.ask && a.last == b.last &&
                a.liquidity == b.liquidity, "synthetic source must be deterministic");
        require(std::isfinite(a.last) && a.bid > 0 && a.ask > a.bid &&
                a.last > a.bid && a.last < a.ask && a.liquidity > 0,
                "synthetic prices and liquidity must be valid");
        require(std::abs((a.ask - a.bid) - 0.02) < 1e-12,
                "synthetic spread must be 0.02");
        if (i == 0) {
            initial_mid = a.last;
        }
        moved = moved || a.last != initial_mid;
    }
    require(moved, "synthetic stress feed must exercise price changes");

    const auto example = bt::load_csv(std::string(BT_SOURCE_DIR) + "/data/example.csv");
    require(example.size() == 10 && example.front().timestamp_ns == 1000 &&
            example.back().last == 99.98, "shipped replay CSV must load correctly");
    std::istringstream valid(std::string(header) +
        "\r\n0,0,99.9,100.1,100,1000\r\n0,0,100,100,100,1\r\n");
    const auto ticks = bt::parse_csv(valid);
    require(ticks.size() == 2 && ticks[0].timestamp_ns == 0 && ticks[1].bid == 100,
            "CSV must permit empty lines, CRLF, zero/repeated timestamps and locked quotes");

    reject("", "line 1");
    reject("wrong,header\n", "header");
    reject(header, "no market ticks");
    reject(std::string(header) + "1,0,100,100,100\n", "six fields");
    reject(std::string(header) + "1,0,100,100,100,1,2\n", "six fields");
    reject(std::string(header) + "1,0,100,100,100,\n", "liquidity");
    for (const auto* timestamp : {"-1", "+1", "1x", "1.0", "18446744073709551616"}) {
        reject(std::string(header) + timestamp + ",0,100,100,100,1\n", "timestamp_ns");
    }
    reject(std::string(header) + "1,1,100,100,100,1\n", "instrument 0");
    reject(std::string(header) + "1,-1,100,100,100,1\n", "instrument");
    reject(std::string(header) + "2,0,100,100,100,1\n1,0,100,100,100,1\n", "line 3");
    reject(std::string(header) + "1,0,101,100,100,1\n", "bid");
    for (const auto* price : {"nan", "inf", "-inf", "0", "-1"}) {
        reject(std::string(header) + "1,0,100,100," + price + ",1\n", "positive");
    }
    reject(std::string(header) + "1,0,100,100,100,0\n", "positive");
    reject(std::string(header) + "1,0,100x,100,100,1\n", "bid");
    reject(std::string(header) + "1,0,1e308,1e308,100,1\n", "maximum");
    reject(std::string(header) + "1,0,100,1e308,100,1\n", "maximum");
    reject(std::string(header) + "1,0,100,100,1e308,1\n", "maximum");
    std::istringstream boundary(std::string(header) + "1,0,1e9,1e9,1e9,1\n");
    require(bt::parse_csv(boundary).front().last == bt::max_market_price,
            "supported maximum price must be inclusive");
}
