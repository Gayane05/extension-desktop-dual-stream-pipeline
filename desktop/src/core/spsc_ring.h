// desktop/src/core/spsc_ring.h
//
// Bounded lock-free queue sitting between WsServer's connection-thread
// callback (producer, calls tryPush from onAudio) and Pipeline's per-stream
// worker thread (consumer, blocks in popWait). One ring per StreamId; see
// Pipeline::rings_. Invariant: exactly one producer and one consumer thread
// per instance -- concurrent producers (or consumers) would race on
// head_/tail_ without the memory-order pairing below being sufficient.
#pragma once
#include <atomic>
#include <optional>
#include <semaphore>
#include <vector>

namespace dsp
{

// Extra semaphore permits close() releases beyond the one the blocked
// consumer needs; see close() below.
inline constexpr int kCloseWakeupCredits = 64;

// Single-producer / single-consumer bounded ring. tryPush never blocks;
// popWait blocks on a semaphore (no busy-wait). Data path is lock-free.
template <typename T>
class SpscRing
{
public:
    // +1 slot: with N usable slots stored in an N+1-capacity buffer, "full"
    // (next == tail) and "empty" (tail == head) are distinguishable purely
    // from head/tail equality, with no separate size counter to keep in sync
    // (which would itself need cross-thread synchronization).
    explicit SpscRing(size_t capacity) : buf_(capacity + 1) {}

    bool tryPush(T&& value)
    {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % buf_.size();
        // acquire pairs with tryPop's release store to tail_: seeing a
        // tail_ value here guarantees the consumer's read of buf_[tail] that
        // preceded it has already happened, so it's safe to now overwrite
        // that slot without racing the consumer's in-progress move-out.
        if (next == tail_.load(std::memory_order_acquire))
        {
            return false;  // Full.
        }
        buf_[head] = std::move(value);
        // release pairs with tryPop's acquire load of head_: publishes both
        // the just-written buf_[head] and this index update together, so the
        // consumer never observes the new head without also observing the
        // data behind it.
        head_.store(next, std::memory_order_release);
        sem_.release();
        return true;
    }

    std::optional<T> popWait()
    {
        for (;;)
        {
            sem_.acquire();
            T value;
            if (tryPop(value))
            {
                return value;
            }
            if (closed_.load(std::memory_order_acquire))
            {
                return std::nullopt;
            }
        }
    }

    void close()
    {
        closed_.store(true, std::memory_order_release);
        // Over-release semaphore with headroom (kCloseWakeupCredits is arbitrary); one
        // credit suffices for the single consumer, but extra permits are harmless because
        // tryPop validates against real head/tail state. Ensures blocked consumer wakes
        // even if spurious wakeups are possible.
        sem_.release(kCloseWakeupCredits);
    }

    bool closed() const { return closed_.load(std::memory_order_acquire); }

private:
    // Mirrors tryPush: acquire on head_ pairs with tryPush's release store
    // (see there), release on tail_ pairs with tryPush's acquire load.
    bool tryPop(T& out)
    {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
        {
            return false;
        }
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
