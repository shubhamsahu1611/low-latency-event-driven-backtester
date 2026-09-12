#include "backtester/data.hpp"
#include "backtester/engine.hpp"
#include "backtester/spsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t queue_capacity = 16384;

struct Options {
    std::string mode = "benchmark";
    std::uint64_t ticks = 20'000'000;
    std::uint64_t warmup = 1'000'000;
    unsigned runs = 3;
    bt::EngineConfig engine{};
    std::string csv;
};

void usage() {
    std::cout << "Usage: backtester [benchmark|queue|replay|risk] [options]\n"
        "  --ticks N              Synthetic ticks per run (default 20000000)\n"
        "                         In risk mode: calculations per run\n"
        "  --runs N               Odd measured repetitions, 1..99 (default 3)\n"
        "  --warmup N             Untimed warmup events (default 1000000)\n"
        "  --risk auto|scalar|avx2 Risk backend (default auto)\n"
        "  --risk-every N         Recompute risk every N ticks (default 256)\n"
        "  --strategy-every N     Strategy decision interval (default 64)\n"
        "  --slippage-bps X       Adverse fill slippage (default 0.5)\n"
        "  --impact-bps X         Quadratic participation coefficient (default 2)\n"
        "  --csv PATH             Required for replay; preload before timing\n"
        "Synthetic and risk benchmarks require a CMake Release build.\n";
}

template <typename T>
T number(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("Invalid numeric argument: " + std::string(text));
    return value;
}

Options parse(int argc, char** argv) {
    Options o;
    int i = 1;
    if (i < argc && argv[i][0] != '-') o.mode = argv[i++];
    if (o.mode != "benchmark" && o.mode != "queue" && o.mode != "replay" && o.mode != "risk")
        throw std::invalid_argument("Unknown mode: " + o.mode);
    for (; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (i + 1 >= argc) throw std::invalid_argument("Missing value for " + std::string(key));
        const std::string_view value = argv[++i];
        if (key == "--ticks") o.ticks = number<std::uint64_t>(value);
        else if (key == "--warmup") o.warmup = number<std::uint64_t>(value);
        else if (key == "--runs") o.runs = number<unsigned>(value);
        else if (key == "--risk-every") o.engine.risk_interval = number<std::uint32_t>(value);
        else if (key == "--strategy-every") o.engine.strategy.interval = number<std::uint32_t>(value);
        else if (key == "--slippage-bps") o.engine.execution.slippage_bps = number<double>(value);
        else if (key == "--impact-bps") o.engine.execution.impact_bps = number<double>(value);
        else if (key == "--csv") o.csv = value;
        else if (key == "--risk") {
            if (value == "auto") o.engine.risk_mode = bt::RiskMode::Auto;
            else if (value == "scalar") o.engine.risk_mode = bt::RiskMode::Scalar;
            else if (value == "avx2") o.engine.risk_mode = bt::RiskMode::Avx2;
            else throw std::invalid_argument("Risk backend must be auto, scalar, or avx2");
        } else throw std::invalid_argument("Unknown option: " + std::string(key));
    }
    if (o.ticks == 0 || o.ticks > 1'000'000'000ULL || o.warmup > 1'000'000'000ULL ||
        o.runs == 0 || o.runs > 99 || o.runs % 2 == 0)
        throw std::invalid_argument("Require 1..1e9 ticks, 0..1e9 warmup, and odd 1..99 runs");
    if ((o.mode == "replay") != !o.csv.empty())
        throw std::invalid_argument("Use --csv PATH with replay only");
    // Validate even queue-mode settings before starting any threads.
    const bt::BacktestEngine validated(o.engine);
    (void)validated;
    return o;
}

struct Result {
    double seconds{};
    std::uint64_t ticks{}, checksum{}, orders{}, fills{}, rejected{}, risk_updates{};
    std::int64_t position{};
    std::size_t skipped{};
    double cash{}, equity{}, pnl{}, realized{}, unrealized{};
    bt::RiskMetrics risk{};
    bool avx2{};
};

// Main thread is the consumer. Initialization, thread creation, joining and
// printing stay outside timing; generation, queue waits and final risk stay in.
Result run_pipeline(const Options& o, std::uint64_t count, const std::vector<bt::Tick>* replay) {
    auto queue = std::make_unique<bt::SpscQueue<bt::Tick, queue_capacity>>();
    bt::BacktestEngine engine(o.engine);
    std::atomic<bool> ready{false}, start{false};
    std::jthread producer([&] {
        bt::SyntheticSource source;
        ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t i = 0; i < count; ++i) {
            const bt::Tick tick = replay ? (*replay)[static_cast<std::size_t>(i)] : source.next();
            while (!queue->try_push(tick)) { /* Bounded queue: backpressure, never drop. */ }
        }
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    Result result;
    const auto begin = Clock::now();
    start.store(true, std::memory_order_release);
    if (o.mode == "queue") {
        for (std::uint64_t i = 0; i < count; ++i) {
            bt::Tick tick;
            while (!queue->try_pop(tick)) {}
            result.checksum += tick.timestamp_ns;
        }
    } else {
        for (std::uint64_t i = 0; i < count; ++i) {
            bt::Tick tick;
            while (!queue->try_pop(tick)) {}
            engine.on_tick(tick);
        }
        engine.finish();
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    producer.join();
    result.ticks = count;
    result.orders = engine.orders();
    result.fills = engine.fills();
    result.rejected = engine.rejected_orders();
    result.risk_updates = engine.risk_updates();
    result.skipped = engine.skipped_returns();
    result.position = engine.portfolio().position();
    result.cash = engine.portfolio().cash();
    result.equity = engine.portfolio().equity(engine.mark());
    result.pnl = engine.portfolio().pnl(engine.mark());
    result.realized = engine.portfolio().realized_pnl();
    result.unrealized = engine.portfolio().unrealized_pnl(engine.mark());
    result.risk = engine.metrics();
    result.avx2 = engine.uses_avx2();
    for (double value : {result.cash, result.equity, result.pnl, result.realized,
                        result.unrealized, result.risk.mean, result.risk.variance,
                        result.risk.volatility, result.risk.sharpe, result.risk.var95}) {
        if (!std::isfinite(value))
            throw std::runtime_error("Input or execution costs exceeded the supported accounting/risk numeric range");
    }
    return result;
}

void report(const Result& r, bool queue_only) {
    if (queue_only) { std::cout << "checksum=" << r.checksum << '\n'; return; }
    std::cout << "orders=" << r.orders << " fills=" << r.fills << " rejected=" << r.rejected
        << " position=" << r.position << " risk_updates=" << r.risk_updates << '\n'
        << "cash=" << r.cash << " equity=" << r.equity << " pnl=" << r.pnl
        << " realized=" << r.realized << " unrealized=" << r.unrealized << '\n'
        << std::setprecision(10) << "sharpe=" << r.risk.sharpe << " var95=" << r.risk.var95
        << " mean_return=" << r.risk.mean << " volatility=" << r.risk.volatility
        << " observations=" << r.risk.observations << " skipped_returns=" << r.skipped << '\n'
        << std::setprecision(6);
}

void risk_benchmark(const Options& o) {
    bt::RollingRisk risk(o.engine.initial_cash, o.engine.risk_mode);
    for (std::size_t i = 0; i < 1024; ++i) risk.observe(1'000'000.0 + static_cast<double>((i * 53) % 997));
    std::vector<double> elapsed;
    double checksum = 0.0;
    for (std::uint64_t i = 0; i < o.warmup; ++i) checksum += risk.calculate(1'000'000.0).var95;
    for (unsigned run = 0; run < o.runs; ++run) {
        const auto begin = Clock::now();
        for (std::uint64_t i = 0; i < o.ticks; ++i) {
            // Change the stream to prevent loop-invariant calculation hoisting.
            risk.observe(1'000'000.0 + static_cast<double>((i * 53) % 997));
            const auto metrics = risk.calculate(1'000'000.0);
            checksum += metrics.sharpe + metrics.var95;
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        elapsed.push_back(seconds);
        std::cout << "run=" << run + 1 << " calculations=" << o.ticks << " elapsed_s=" << seconds
            << " calculations_per_s=" << static_cast<double>(o.ticks) / seconds << '\n';
    }
    std::sort(elapsed.begin(), elapsed.end());
    std::cout << "risk_backend=" << (risk.uses_avx2() ? "avx2" : "scalar")
        << " median_calculations_per_s=" << static_cast<double>(o.ticks) / elapsed[elapsed.size() / 2]
        << " checksum=" << checksum << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
        const auto options = parse(argc, argv);
        if (!BT_BENCHMARK_BUILD && options.mode != "replay")
            throw std::runtime_error("Benchmarks require -DCMAKE_BUILD_TYPE=Release (or --config Release)");
        std::cout << std::fixed << std::setprecision(6)
            << "mode=" << options.mode << " build=" << BT_BUILD_TYPE
            << " hardware_threads=" << std::thread::hardware_concurrency()
            << " tick_bytes=" << sizeof(bt::Tick) << " queue_capacity=" << queue_capacity << '\n'
            << "risk_backend=" << (bt::BacktestEngine(options.engine).uses_avx2() ? "avx2" : "scalar")
            << " window=1024 risk_every=" << options.engine.risk_interval
            << " strategy_every=" << options.engine.strategy.interval
            << " slippage_bps=" << options.engine.execution.slippage_bps
            << " impact_bps=" << options.engine.execution.impact_bps << '\n';
        if (options.mode == "risk") { risk_benchmark(options); return 0; }
        std::vector<bt::Tick> replay;
        if (options.mode == "replay") replay = bt::load_csv(options.csv);
        const auto count = options.mode == "replay" ? static_cast<std::uint64_t>(replay.size()) : options.ticks;
        const auto* replay_ptr = options.mode == "replay" ? &replay : nullptr;
        const auto warmup = replay_ptr ? std::min(options.warmup, count) : options.warmup;
        if (warmup) (void)run_pipeline(options, warmup, replay_ptr);
        std::cout << "timing=" << (options.mode == "queue"
            ? "producer_generation+queue+checksum"
            : "producer_generation_or_replay+queue+engine+final_risk")
            << " warmup_ticks=" << warmup << '\n';
        std::vector<Result> results;
        results.reserve(options.runs);
        for (unsigned i = 0; i < options.runs; ++i) {
            const auto r = run_pipeline(options, count, replay_ptr);
            results.push_back(r);
            std::cout << "run=" << i + 1 << " ticks=" << r.ticks << " elapsed_s=" << r.seconds
                << " ticks_per_s=" << static_cast<double>(r.ticks) / r.seconds
                << " mticks_per_s=" << static_cast<double>(r.ticks) / r.seconds / 1e6 << '\n';
        }
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) { return a.seconds < b.seconds; });
        const auto& median = results[results.size() / 2];
        std::cout << "median_ticks_per_s=" << static_cast<double>(count) / median.seconds
            << " median_mticks_per_s=" << static_cast<double>(count) / median.seconds / 1e6 << '\n';
        report(median, options.mode == "queue");
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
