// desktop/tests/test_spsc_ring.cpp
//
// Unit/concurrency tests for core/spsc_ring.h: FIFO ordering, full/empty
// edge cases, and cross-thread push/pop correctness under popWait's blocking
// semaphore wait.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "core/spsc_ring.h"

using dsp::SpscRing;

TEST(SpscRing, PushPopFifo)
{
    SpscRing<int> ring(4);
    EXPECT_TRUE(ring.tryPush(1));
    EXPECT_TRUE(ring.tryPush(2));
    EXPECT_EQ(ring.popWait().value(), 1);
    EXPECT_EQ(ring.popWait().value(), 2);
}

TEST(SpscRing, TryPushFailsWhenFull)
{
    SpscRing<int> ring(2);
    EXPECT_TRUE(ring.tryPush(1));
    EXPECT_TRUE(ring.tryPush(2));
    EXPECT_FALSE(ring.tryPush(3));  // Full: incoming dropped by caller.
}

TEST(SpscRing, CloseUnblocksConsumerAndDrains)
{
    SpscRing<int> ring(4);
    ring.tryPush(42);
    ring.close();
    EXPECT_EQ(ring.popWait().value(), 42);     // Drains remaining item.
    EXPECT_FALSE(ring.popWait().has_value());  // Then reports closed.
}

TEST(SpscRing, ThreadedSmoke)
{
    SpscRing<int> ring(1024);
    constexpr int kN = 100000;
    std::thread producer([&] {
        for (int i = 0; i < kN;)
        {
            if (ring.tryPush(std::move(i)))
            {
                ++i;
            }
        }
        ring.close();
    });
    int expected = 0;
    while (auto poppedValue = ring.popWait())
    {
        EXPECT_EQ(*poppedValue, expected++);
    }
    producer.join();
    EXPECT_EQ(expected, kN);
}

TEST(SpscRing, CloseWakesBlockedConsumer)
{
    auto ring = std::make_shared<SpscRing<int>>(4);
    auto started = std::make_shared<std::atomic<bool>>(false);
    std::promise<std::optional<int>> resultPromise;
    auto fut = resultPromise.get_future();
    std::thread([ring, started, promise = std::move(resultPromise)]() mutable {
        *started = true;
        promise.set_value(ring->popWait());
    }).detach();
    while (!*started)
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // Let it park.
    ring->close();
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "close() failed to wake the blocked consumer";
    EXPECT_FALSE(fut.get().has_value());
}
