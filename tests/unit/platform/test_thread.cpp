// RT-INIT-001: platform thread / mutex unit tests
#include <gtest/gtest.h>
#include "switchrecomp/platform/thread.h"
#include <vector>
#include <memory>

using namespace sr::platform;

TEST(PlatformThread, MutexConcurrentIncrement) {
    Mutex mtx;
    int counter = 0;
    const int kThreads = 10;
    const int kIters = 10000;

    std::vector<std::unique_ptr<Thread>> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.push_back(std::make_unique<Thread>([&mtx, &counter, kIters]() {
            for (int i = 0; i < kIters; ++i) {
                MutexGuard g(mtx);
                ++counter;
            }
        }));
    }

    for (auto& th : threads) th->Join();

    EXPECT_EQ(counter, kThreads * kIters);
}
