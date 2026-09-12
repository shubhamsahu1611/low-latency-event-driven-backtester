#pragma once

#include <array>
#include <cassert>
#include <cstddef>

namespace bt {

// Consumer-thread-only pool: objects are constructed once and never allocated
// during acquire/release. Reuse preserves contents; callers overwrite fields.
// Release only a currently acquired pointer from this pool, exactly once.
// All acquired objects must cease to be used before their pool is destroyed.
template <typename T, std::size_t Capacity>
class ObjectPool {
    static_assert(Capacity > 0, "Object pool capacity must be nonzero");

    std::array<T, Capacity> objects_{};
    std::array<std::size_t, Capacity> free_indices_{};
    std::size_t free_count_{Capacity};

public:
    ObjectPool() {
        for (std::size_t i = 0; i < Capacity; ++i) {
            free_indices_[i] = Capacity - 1 - i;
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    [[nodiscard]] T* acquire() noexcept {
        if (free_count_ == 0) {
            return nullptr;
        }
        return &objects_[free_indices_[--free_count_]];
    }

    void release(T* object) noexcept {
        assert(object != nullptr && free_count_ < Capacity);
        const auto index = static_cast<std::size_t>(object - objects_.data());
        assert(index < Capacity);
        free_indices_[free_count_++] = index;
    }

    [[nodiscard]] std::size_t available() const noexcept { return free_count_; }
};

} // namespace bt
