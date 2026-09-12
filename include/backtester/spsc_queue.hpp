#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace bt {

// Exactly one thread calls try_push and exactly one calls try_pop. The queue
// must outlive both threads. A failed operation leaves the queue unchanged.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "SPSC capacity must be a nonzero power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSC events must be trivially copyable");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "This queue requires lock-free size_t atomics");

    // Each thread writes only its own state. Separate cache lines prevent
    // false sharing; cached remote indices avoid most cross-core atomic reads.
    struct alignas(64) ProducerState {
        std::atomic<std::size_t> head{0};
        std::size_t cached_tail{0};
    };
    struct alignas(64) ConsumerState {
        std::atomic<std::size_t> tail{0};
        std::size_t cached_head{0};
    };

    std::array<T, Capacity> slots_{};
    ProducerState producer_{};
    ConsumerState consumer_{};

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    [[nodiscard]] bool try_push(const T& event) noexcept {
        const auto head = producer_.head.load(std::memory_order_relaxed);
        if (head - producer_.cached_tail == Capacity) {
            producer_.cached_tail = consumer_.tail.load(std::memory_order_acquire);
            if (head - producer_.cached_tail == Capacity) {
                return false;
            }
        }
        // Acquiring tail ensures the consumer has finished reading this slot.
        slots_[head & (Capacity - 1)] = event;
        producer_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& event) noexcept {
        const auto tail = consumer_.tail.load(std::memory_order_relaxed);
        if (tail == consumer_.cached_head) {
            consumer_.cached_head = producer_.head.load(std::memory_order_acquire);
            if (tail == consumer_.cached_head) {
                return false;
            }
        }
        // Acquiring head makes the producer's preceding slot write visible.
        event = slots_[tail & (Capacity - 1)];
        consumer_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }
};

} // namespace bt
