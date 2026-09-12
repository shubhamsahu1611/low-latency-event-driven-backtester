#include "backtester/object_pool.hpp"
#include "backtester/spsc_queue.hpp"

#include <cstdint>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_fifo_and_wrap() {
    bt::SpscQueue<std::uint64_t, 4> queue;
    std::uint64_t value = 123;
    require(!queue.try_pop(value), "new queue must be empty");
    require(value == 123, "empty pop must not overwrite its output");

    for (std::uint64_t base = 0; base < 40'000; base += 4) {
        for (std::uint64_t i = 0; i < 4; ++i) {
            require(queue.try_push(base + i), "all queue slots must be usable");
        }
        require(!queue.try_push(999), "push into a full queue must fail");
        for (std::uint64_t i = 0; i < 4; ++i) {
            require(queue.try_pop(value), "queued event must be available");
            require(value == base + i, "queue must preserve FIFO across wraparound");
        }
        require(!queue.try_pop(value), "drained queue must be empty");
    }

    bt::SpscQueue<std::uint64_t, 1> single_slot;
    require(single_slot.try_push(42), "capacity-one queue must accept an event");
    require(!single_slot.try_push(43), "capacity-one queue must report full");
    require(single_slot.try_pop(value) && value == 42, "capacity-one FIFO failed");
    require(single_slot.try_push(43), "capacity-one slot must be reusable");
    require(single_slot.try_pop(value) && value == 43, "capacity-one reuse failed");
}

void test_concurrent_transfer() {
    struct Event {
        std::uint64_t sequence;
        std::uint64_t complement;
    };
    bt::SpscQueue<Event, 1024> queue;
    constexpr std::uint64_t count = 1'000'000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            const Event event{i, ~i};
            while (!queue.try_push(event)) {
                std::this_thread::yield();
            }
        }
    });

    bool correct = true;
    for (std::uint64_t expected = 0; expected < count; ++expected) {
        Event event{};
        while (!queue.try_pop(event)) {
            std::this_thread::yield();
        }
        correct = correct && event.sequence == expected && event.complement == ~expected;
    }
    producer.join();
    require(correct, "concurrent queue lost, duplicated, reordered, or corrupted an event");
    Event event{};
    require(!queue.try_pop(event), "concurrent transfer must leave the queue empty");
}

void test_pool_reuse() {
    struct Order {
        std::uint64_t id{};
        double quantity{};
    };
    bt::ObjectPool<Order, 3> pool;
    require(pool.available() == 3, "pool must begin fully available");
    auto* first = pool.acquire();
    auto* second = pool.acquire();
    auto* third = pool.acquire();
    require(first && second && third, "pool must provide all preallocated objects");
    require(first != second && first != third && second != third,
            "simultaneously acquired pool objects must be distinct");
    require(pool.available() == 0 && pool.acquire() == nullptr,
            "exhausted pool must return nullptr");

    second->id = 77;
    pool.release(second);
    require(pool.available() == 1, "release must restore pool availability");
    auto* reused = pool.acquire();
    require(reused == second && reused->id == 77, "pool must reuse its preconstructed object");
    pool.release(first);
    pool.release(reused);
    pool.release(third);
    require(pool.available() == 3, "all released pool slots must be available");

    for (std::uint64_t i = 0; i < 10'000; ++i) {
        auto* order = pool.acquire();
        require(order != nullptr, "pool reuse must not exhaust capacity");
        order->id = i;
        order->quantity = static_cast<double>(i);
        pool.release(order);
    }
    require(pool.available() == 3, "repeated reuse must preserve pool capacity");
}

} // namespace

void test_queue_pool() {
    test_fifo_and_wrap();
    test_concurrent_transfer();
    test_pool_reuse();
}
