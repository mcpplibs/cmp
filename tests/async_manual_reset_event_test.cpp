#include <gtest/gtest.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::AsyncManualResetEvent;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;

Task<void> increment_after_event(
    AsyncManualResetEvent& event,
    int& completed) {
    co_await event;
    ++completed;
}

Task<void> record_after_event(
    AsyncManualResetEvent& event,
    int value,
    std::vector<int>& order) {
    co_await event;
    order.push_back(value);
}

Task<void> observe_published_value(
    AsyncManualResetEvent& event,
    const int& published,
    int& observed,
    std::thread::id& resumedThread) {
    co_await event;
    observed = published;
    resumedThread = std::this_thread::get_id();
}

Task<void> wait_with_cancellation(
    AsyncManualResetEvent& event,
    std::stop_token stopToken,
    bool& signalled,
    bool& cancelled,
    int& resumeCount,
    std::thread::id& resumedThread) {
    try {
        co_await event.wait(stopToken);
        signalled = true;
    } catch (const OperationCancelled&) {
        cancelled = true;
    }

    ++resumeCount;
    resumedThread = std::this_thread::get_id();
}

Task<void> signal_next(
    AsyncManualResetEvent& current,
    AsyncManualResetEvent& next,
    int& completed) {
    co_await current;
    ++completed;
    next.set();
}

Task<void> finish_chain(
    AsyncManualResetEvent& event,
    int& completed) {
    co_await event;
    ++completed;
}

Task<int> run_signal_chain(int count) {
    auto events = std::make_unique<AsyncManualResetEvent[]>(count);
    int completed { 0 };
    TaskGroup group {};

    for (int index { 0 }; index + 1 < count; ++index) {
        group.spawn(signal_next(
            events[index],
            events[index + 1],
            completed));
    }
    group.spawn(finish_chain(events[count - 1], completed));

    events[0].set();
    co_await group.join();
    co_return completed;
}

Task<int> run_many_waiters(int count) {
    AsyncManualResetEvent event {};
    int completed { 0 };
    TaskGroup group {};

    for (int index { 0 }; index < count; ++index) {
        group.spawn(increment_after_event(event, completed));
    }

    event.set();
    co_await group.join();
    co_return completed;
}

TEST(CmpAsyncManualResetEventTest, SupportsInitialStateAndReset) {
    static_assert(!std::copy_constructible<AsyncManualResetEvent>);
    static_assert(!std::move_constructible<AsyncManualResetEvent>);

    AsyncManualResetEvent unset {};
    AsyncManualResetEvent set { true };

    EXPECT_FALSE(unset.is_set());
    EXPECT_TRUE(set.is_set());

    unset.reset();
    EXPECT_FALSE(unset.is_set());
    unset.set();
    EXPECT_TRUE(unset.is_set());
    unset.set();
    EXPECT_TRUE(unset.is_set());
    unset.reset();
    EXPECT_FALSE(unset.is_set());
}

TEST(CmpAsyncManualResetEventTest, SetStateContinuesInlineUntilReset) {
    RunLoop loop {};
    AsyncManualResetEvent event { true };
    int completed { 0 };

    loop.run(increment_after_event(event, completed));
    loop.run(increment_after_event(event, completed));
    EXPECT_EQ(completed, 2);

    event.reset();
    TaskGroup group {};
    group.spawn(increment_after_event(event, completed));
    EXPECT_EQ(completed, 2);

    event.set();
    loop.run(group.join());
    EXPECT_EQ(completed, 3);
}

TEST(CmpAsyncManualResetEventTest, SetPublishesAndResumesOnSetterThread) {
    RunLoop loop {};
    AsyncManualResetEvent event {};
    TaskGroup group {};
    int published { 0 };
    int observed { 0 };
    std::thread::id resumedThread {};
    std::thread::id setterThread {};

    group.spawn(observe_published_value(
        event,
        published,
        observed,
        resumedThread));

    std::jthread setter { [&] {
        setterThread = std::this_thread::get_id();
        published = 42;
        event.set();
    } };
    setter.join();
    loop.run(group.join());

    EXPECT_EQ(observed, 42);
    EXPECT_EQ(resumedThread, setterThread);
}

TEST(CmpAsyncManualResetEventTest, SetResumesWaitersInFifoOrder) {
    RunLoop loop {};
    AsyncManualResetEvent event {};
    TaskGroup group {};
    std::vector<int> order {};

    group.spawn(record_after_event(event, 1, order));
    group.spawn(record_after_event(event, 2, order));
    group.spawn(record_after_event(event, 3, order));

    event.set();
    loop.run(group.join());

    EXPECT_EQ(order, (std::vector<int> { 1, 2, 3 }));
}

TEST(CmpAsyncManualResetEventTest, PreCancelledWaitWinsDeterministically) {
    RunLoop loop {};
    AsyncManualResetEvent event { true };
    std::stop_source stopSource {};
    bool signalled { false };
    bool cancelled { false };
    int resumeCount { 0 };
    std::thread::id resumedThread {};
    stopSource.request_stop();

    loop.run(wait_with_cancellation(
        event,
        stopSource.get_token(),
        signalled,
        cancelled,
        resumeCount,
        resumedThread));

    EXPECT_FALSE(signalled);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(resumeCount, 1);
    EXPECT_EQ(resumedThread, std::this_thread::get_id());
}

TEST(CmpAsyncManualResetEventTest, PendingCancellationRemovesOnlyItsWaiter) {
    RunLoop loop {};
    AsyncManualResetEvent event {};
    TaskGroup group {};
    std::stop_source stopSource {};
    std::vector<int> order {};
    bool signalled { false };
    bool cancelled { false };
    int resumeCount { 0 };
    std::thread::id resumedThread {};
    const auto cancellationThread = std::this_thread::get_id();

    group.spawn(record_after_event(event, 1, order));
    group.spawn(wait_with_cancellation(
        event,
        stopSource.get_token(),
        signalled,
        cancelled,
        resumeCount,
        resumedThread));
    group.spawn(record_after_event(event, 3, order));

    stopSource.request_stop();
    event.set();
    loop.run(group.join());

    EXPECT_FALSE(signalled);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(resumeCount, 1);
    EXPECT_EQ(resumedThread, cancellationThread);
    EXPECT_EQ(order, (std::vector<int> { 1, 3 }));
}

TEST(CmpAsyncManualResetEventTest, CancellationResumesOnRequestThread) {
    RunLoop loop {};
    AsyncManualResetEvent event {};
    TaskGroup group {};
    std::stop_source stopSource {};
    bool signalled { false };
    bool cancelled { false };
    int resumeCount { 0 };
    std::thread::id resumedThread {};
    std::thread::id cancellationThread {};

    group.spawn(wait_with_cancellation(
        event,
        stopSource.get_token(),
        signalled,
        cancelled,
        resumeCount,
        resumedThread));

    std::jthread canceller { [&] {
        cancellationThread = std::this_thread::get_id();
        stopSource.request_stop();
    } };
    canceller.join();
    loop.run(group.join());

    EXPECT_FALSE(signalled);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(resumeCount, 1);
    EXPECT_EQ(resumedThread, cancellationThread);
}

TEST(CmpAsyncManualResetEventTest, SetCancellationRaceResumesExactlyOnce) {
    constexpr int ITERATIONS { 1'000 };
    RunLoop loop {};
    int setWins { 0 };
    int cancellationWins { 0 };

    for (int iteration { 0 }; iteration < ITERATIONS; ++iteration) {
        AsyncManualResetEvent event {};
        TaskGroup group {};
        std::stop_source stopSource {};
        std::latch start { 1 };
        bool signalled { false };
        bool cancelled { false };
        int resumeCount { 0 };
        std::thread::id resumedThread {};

        group.spawn(wait_with_cancellation(
            event,
            stopSource.get_token(),
            signalled,
            cancelled,
            resumeCount,
            resumedThread));

        std::jthread setter { [&] {
            start.wait();
            event.set();
        } };
        std::jthread canceller { [&] {
            start.wait();
            stopSource.request_stop();
        } };
        start.count_down();
        setter.join();
        canceller.join();
        loop.run(group.join());

        ASSERT_EQ(resumeCount, 1);
        ASSERT_NE(signalled, cancelled);
        setWins += signalled ? 1 : 0;
        cancellationWins += cancelled ? 1 : 0;
    }

    EXPECT_EQ(setWins + cancellationWins, ITERATIONS);
}

TEST(CmpAsyncManualResetEventTest, ManyWaitersDoNotGrowTheNativeStack) {
    constexpr int WAITER_COUNT { 50'000 };
    RunLoop loop {};

    EXPECT_EQ(loop.run(run_many_waiters(WAITER_COUNT)), WAITER_COUNT);
}

TEST(CmpAsyncManualResetEventTest, NestedSignalsDoNotGrowTheNativeStack) {
    constexpr int EVENT_COUNT { 20'000 };
    RunLoop loop {};

    EXPECT_EQ(loop.run(run_signal_chain(EVENT_COUNT)), EVENT_COUNT);
}

}  // namespace
