#include <gtest/gtest.h>
#include <thread>
#include <future>
#include <memory>
#include <atomic>
#include <chrono>
#include "core/spsc_ring.h"

using dsp::SpscRing;

TEST(SpscRing, PushPopFifo) {
    SpscRing<int> q(4);
    EXPECT_TRUE(q.tryPush(1));
    EXPECT_TRUE(q.tryPush(2));
    EXPECT_EQ(q.popWait().value(), 1);
    EXPECT_EQ(q.popWait().value(), 2);
}

TEST(SpscRing, TryPushFailsWhenFull) {
    SpscRing<int> q(2);
    EXPECT_TRUE(q.tryPush(1));
    EXPECT_TRUE(q.tryPush(2));
    EXPECT_FALSE(q.tryPush(3));  // full: incoming dropped by caller
}

TEST(SpscRing, CloseUnblocksConsumerAndDrains) {
    SpscRing<int> q(4);
    q.tryPush(42);
    q.close();
    EXPECT_EQ(q.popWait().value(), 42);      // drains remaining item
    EXPECT_FALSE(q.popWait().has_value());   // then reports closed
}

TEST(SpscRing, ThreadedSmoke) {
    SpscRing<int> q(1024);
    constexpr int kN = 100000;
    std::thread producer([&] {
        for (int i = 0; i < kN;) if (q.tryPush(std::move(i))) ++i;
        q.close();
    });
    int expected = 0;
    while (auto v = q.popWait()) { EXPECT_EQ(*v, expected++); }
    producer.join();
    EXPECT_EQ(expected, kN);
}

TEST(SpscRing, CloseWakesBlockedConsumer) {
    auto q = std::make_shared<SpscRing<int>>(4);
    auto started = std::make_shared<std::atomic<bool>>(false);
    std::promise<std::optional<int>> resultPromise;
    auto fut = resultPromise.get_future();
    std::thread([q, started, p = std::move(resultPromise)]() mutable {
        *started = true;
        p.set_value(q->popWait());
    }).detach();
    while (!*started) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let it park
    q->close();
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "close() failed to wake the blocked consumer";
    EXPECT_FALSE(fut.get().has_value());
}
