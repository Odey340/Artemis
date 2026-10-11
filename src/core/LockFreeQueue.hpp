#pragma once
// Bounded single-producer / single-consumer lock-free ring buffer.
//
// Carried over from the original Artemis tick engine: the market-data reader
// thread is the only producer and the strategy worker is the only consumer,
// so a pair of acquire/release atomics is all the synchronisation we need.

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace artemis {

// Fixed at 64 bytes (x86-64 / most ARM cores) to keep the layout ABI-stable.
inline constexpr std::size_t kCacheLine = 64;

template <typename T>
class LockFreeQueue {
public:
    // Capacity is rounded up to the next power of two (one slot is kept free).
    explicit LockFreeQueue(std::size_t capacity = 1024) {
        std::size_t cap = 2;
        while (cap < capacity + 1) cap <<= 1;
        mask_ = cap - 1;
        slots_ = std::make_unique<Slot[]>(cap);
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    ~LockFreeQueue() {
        while (tryPop()) {
        }
    }

    // Producer side. Returns false when the queue is full.
    template <typename U>
    bool tryPush(U&& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire)) return false;  // full
        new (&slots_[head].storage) T(std::forward<U>(value));
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns nullopt when empty.
    std::optional<T> tryPop() {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return std::nullopt;  // empty
        T* p = std::launder(reinterpret_cast<T*>(&slots_[tail].storage));
        std::optional<T> out(std::move(*p));
        p->~T();
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return out;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    std::size_t sizeApprox() const {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & mask_;
    }

    std::size_t capacity() const { return mask_; }

private:
    struct Slot {
        typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
    };

    std::size_t mask_ = 0;
    std::unique_ptr<Slot[]> slots_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
};

}  // namespace artemis
