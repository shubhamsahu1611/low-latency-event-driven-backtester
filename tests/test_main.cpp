#include "backtester/data.hpp"
#include "backtester/engine.hpp"
#include "backtester/spsc_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#include <malloc.h>
#endif

void test_queue_pool();
void test_trading();
void test_risk();
void test_data();

namespace {

// Counts replaceable C++ allocation calls on this thread, not C malloc calls.
// Startup, test assertions, reporting, and thread creation are outside tracking.
thread_local bool track_allocations = false;
thread_local std::size_t allocation_count = 0;

void* allocate(std::size_t bytes, std::size_t alignment = 0) {
    if (track_allocations) ++allocation_count;
    if (bytes == 0) bytes = 1;
#if !defined(_WIN32)
    if (alignment != 0) {
        if (bytes > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
            throw std::bad_alloc();
        }
        bytes = (bytes + alignment - 1) / alignment * alignment;
    }
#endif
    for (;;) {
        void* memory = nullptr;
        if (alignment == 0) memory = std::malloc(bytes);
#if defined(_WIN32)
        else memory = _aligned_malloc(bytes, alignment);
#else
        else memory = std::aligned_alloc(alignment, bytes);
#endif
        if (memory != nullptr) return memory;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

void free_aligned(void* memory) noexcept {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

class AllocationScope {
public:
    AllocationScope() noexcept {
        allocation_count = 0;
        track_allocations = true;
    }
    ~AllocationScope() { track_allocations = false; }
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;
    std::size_t stop() noexcept {
        track_allocations = false;
        return allocation_count;
    }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

// All standard replaceable forms share the same counter and matching free.
void* operator new(std::size_t bytes) { return allocate(bytes); }
void* operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory, std::align_val_t) noexcept { free_aligned(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { free_aligned(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { free_aligned(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { free_aligned(memory); }

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return ::operator new[](bytes); } catch (...) { return nullptr; }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new[](bytes, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    free_aligned(memory);
}
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    free_aligned(memory);
}

namespace {

void test_allocation_hooks() {
    AllocationScope scope;
    void* single = ::operator new(8);
    void* array = ::operator new[](8);
    void* aligned_single = ::operator new(64, std::align_val_t{64});
    void* aligned_array = ::operator new[](64, std::align_val_t{64});
    ::operator delete(single);
    ::operator delete[](array);
    ::operator delete(aligned_single, std::align_val_t{64});
    ::operator delete[](aligned_array, std::align_val_t{64});
    require(scope.stop() == 4, "allocation instrumentation failed its self-check");
}

void test_no_hot_path_allocations(bt::RiskMode mode) {
    constexpr std::size_t events = 200'000;
    bt::EngineConfig config;
    config.risk_mode = mode;
    config.risk_interval = 64;
    bt::BacktestEngine engine(config);
    bt::SpscQueue<bt::Tick, 1024> queue;
    std::atomic<bool> start{false};
    std::size_t producer_allocations = 0;
    std::thread producer([&] {
        bt::SyntheticSource source;
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        AllocationScope scope;
        for (std::size_t i = 0; i < events; ++i) {
            const bt::Tick tick = source.next();
            while (!queue.try_push(tick)) {}
        }
        producer_allocations = scope.stop();
    });

    // Both the thread and the complete engine exist before either guard starts.
    bt::Tick tick;
    AllocationScope scope;
    start.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < events; ++i) {
        while (!queue.try_pop(tick)) {}
        engine.on_tick(tick);
    }
    engine.finish();
    const std::size_t consumer_allocations = scope.stop();
    producer.join();

    require(producer_allocations == 0, "synthetic producer allocated in its hot path");
    require(consumer_allocations == 0, "backtesting consumer allocated in its hot path");
    require(engine.ticks() == events, "allocation test did not process every event");
    require(engine.orders() > 0 && engine.fills() == engine.orders(),
            "allocation test did not exercise execution and pooled order reuse");
    require(engine.risk_updates() > 0 && engine.metrics().observations == bt::risk_window_capacity,
            "allocation test did not exercise rolling risk calculations");
}

} // namespace

int main() {
    try {
        test_queue_pool();
        std::cout << "[PASS] SPSC queue and object pool\n";
        test_trading();
        std::cout << "[PASS] Strategy, execution, portfolio, and engine\n";
        test_risk();
        std::cout << "[PASS] Rolling risk and scalar/AVX2 verification\n";
        test_data();
        std::cout << "[PASS] Synthetic source and CSV validation\n";
        test_allocation_hooks();
        test_no_hot_path_allocations(bt::RiskMode::Scalar);
        std::cout << "[PASS] 200,000 ticks with zero hot-path C++ allocations (scalar risk)\n";
        test_no_hot_path_allocations(bt::RiskMode::Auto);
        std::cout << "[PASS] 200,000 ticks with zero hot-path C++ allocations (automatic risk)\n";
        std::cout << "All tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[FAIL] Unexpected non-standard exception\n";
    }
    return 1;
}
