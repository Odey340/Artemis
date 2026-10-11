#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>

#include "core/LockFreeQueue.hpp"

using namespace artemis;

TEST(LockFreeQueue, FifoOrder) {
    LockFreeQueue<int> q(8);
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(q.tryPush(i));
    for (int i = 0; i < 5; ++i) EXPECT_EQ(q.tryPop().value(), i);
    EXPECT_FALSE(q.tryPop().has_value());
    EXPECT_TRUE(q.empty());
}

TEST(LockFreeQueue, ReportsFull) {
    LockFreeQueue<int> q(3);  // rounded to 4 slots, 3 usable
    EXPECT_EQ(q.capacity(), 3u);
    EXPECT_TRUE(q.tryPush(1));
    EXPECT_TRUE(q.tryPush(2));
    EXPECT_TRUE(q.tryPush(3));
    EXPECT_FALSE(q.tryPush(4));
    EXPECT_EQ(q.sizeApprox(), 3u);
}

TEST(LockFreeQueue, FailedPushDoesNotConsumeValue) {
    LockFreeQueue<std::string> q(1);
    std::string a = "first", b = "second";
    EXPECT_TRUE(q.tryPush(std::move(a)));
    EXPECT_FALSE(q.tryPush(std::move(b)));
    EXPECT_EQ(b, "second");  // still intact
}

TEST(LockFreeQueue, MoveOnlyAndDestructors) {
    auto counter = std::make_shared<int>(0);
    {
        LockFreeQueue<std::shared_ptr<int>> q(16);
        for (int i = 0; i < 10; ++i) q.tryPush(counter);
        EXPECT_EQ(counter.use_count(), 11);
        q.tryPop();
        EXPECT_EQ(counter.use_count(), 10);
    }
    EXPECT_EQ(counter.use_count(), 1);  // queue destructor released the rest

    LockFreeQueue<std::unique_ptr<int>> u(4);
    u.tryPush(std::make_unique<int>(7));
    EXPECT_EQ(*u.tryPop().value(), 7);
}

TEST(LockFreeQueue, ProducerConsumerThreads) {
    LockFreeQueue<long> q(1024);
    constexpr long kN = 1'000'000;
    std::thread producer([&] {
        for (long i = 1; i <= kN; ++i)
            while (!q.tryPush(i)) std::this_thread::yield();
    });
    long sum = 0, expected = 1, received = 0;
    bool ordered = true;
    while (received < kN) {
        if (auto v = q.tryPop()) {
            ordered &= (*v == expected++);
            sum += *v;
            ++received;
        }
    }
    producer.join();
    EXPECT_TRUE(ordered);
    EXPECT_EQ(sum, kN * (kN + 1) / 2);
}
