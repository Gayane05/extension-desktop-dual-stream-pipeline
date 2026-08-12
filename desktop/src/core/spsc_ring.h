#pragma once
#include <atomic>
#include <optional>
#include <semaphore>
#include <vector>

namespace dsp {

// Single-producer / single-consumer bounded ring. tryPush never blocks;
// popWait blocks on a semaphore (no busy-wait). Data path is lock-free.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity) : buf_(capacity + 1) {}

    bool tryPush(T&& v) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % buf_.size();
        if (next == tail_.load(std::memory_order_acquire)) return false;  // full
        buf_[head] = std::move(v);
        head_.store(next, std::memory_order_release);
        sem_.release();
        return true;
    }

    std::optional<T> popWait() {
        for (;;) {
            sem_.acquire();
            T v;
            if (tryPop(v)) return v;
            if (closed_.load(std::memory_order_acquire)) return std::nullopt;
        }
    }

    void close() {
        closed_.store(true, std::memory_order_release);
        sem_.release(64);  // over-release so a waiting consumer always wakes
    }

    bool closed() const { return closed_.load(std::memory_order_acquire); }

private:
    bool tryPop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = std::move(buf_[tail]);
        tail_.store((tail + 1) % buf_.size(), std::memory_order_release);
        return true;
    }

    std::vector<T> buf_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
    std::atomic<bool> closed_{false};
    std::counting_semaphore<> sem_{0};
};

}  // namespace dsp
