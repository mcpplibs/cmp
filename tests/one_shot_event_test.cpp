#include <gtest/gtest.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::OneShotEvent;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;

using Scheduler = RunLoop::Scheduler;

Task<void> observe_event(
    OneShotEvent& event,
    int& value,
    std::thread::id& resumedThread) {
    co_await event;
    value = 42;
    resumedThread = std::this_thread::get_id();
    co_return;
}

Task<void> increment_after_event(
    OneShotEvent& event,
    int& completed) {
    co_await event;
    ++completed;
    co_return;
}

Task<void> observe_published_value(
    OneShotEvent& event,
    const int& published,
    int& observed) {
    co_await event;
    observed = published;
    co_return;
}

Task<void> return_to_scheduler(
    OneShotEvent& event,
    Scheduler scheduler,
    std::array<std::thread::id, 2>& threads) {
    co_await event;
    threads[0] = std::this_thread::get_id();
    co_await scheduler.schedule();
    threads[1] = std::this_thread::get_id();
    co_return;
}

Task<std::array<std::thread::id, 2>> run_scheduler_return(
    Scheduler scheduler) {
    std::array<std::thread::id, 2> threads {};
    OneShotEvent event {};
    TaskGroup group {};
    std::jthread setter {};

    group.spawn(return_to_scheduler(event, scheduler, threads));
    setter = std::jthread { [&event] {
        event.set();
    } };

    co_await group.join();
    co_return threads;
}

Task<int> run_many_waiters(int count) {
    int completed { 0 };
    OneShotEvent event {};
    TaskGroup group {};

    for (int index { 0 }; index < count; ++index) {
        group.spawn(increment_after_event(event, completed));
    }

    event.set();
    co_await group.join();
    co_return completed;
}

TEST(CmpOneShotEventTest, StartsUnsetAndIsImmovable) {
    static_assert(!std::copy_constructible<OneShotEvent>);
    static_assert(!std::move_constructible<OneShotEvent>);

    OneShotEvent event {};
    EXPECT_FALSE(event.is_set());
    event.set();
    EXPECT_TRUE(event.is_set());
}

TEST(CmpOneShotEventTest, SetBeforeWaitContinuesInline) {
    RunLoop loop {};
    OneShotEvent event {};
    int value { 0 };
    std::thread::id resumedThread {};
    const auto callingThread = std::this_thread::get_id();

    event.set();
    loop.run(observe_event(event, value, resumedThread));

    EXPECT_EQ(value, 42);
    EXPECT_EQ(resumedThread, callingThread);
}

TEST(CmpOneShotEventTest, SuspendedWaiterResumesOnSetterThread) {
    RunLoop loop {};
    int value { 0 };
    std::thread::id resumedThread {};
    std::thread::id setterThread {};
    OneShotEvent event {};
    TaskGroup group {};

    group.spawn(observe_event(event, value, resumedThread));
    EXPECT_EQ(value, 0);

    std::jthread setter { [&event, &setterThread] {
        setterThread = std::this_thread::get_id();
        event.set();
    } };
    setter.join();
    loop.run(group.join());

    EXPECT_EQ(value, 42);
    EXPECT_EQ(resumedThread, setterThread);
}

TEST(CmpOneShotEventTest, ResumesEveryWaiterOnceAndSetIsIdempotent) {
    RunLoop loop {};
    OneShotEvent event {};
    TaskGroup group {};
    int completed { 0 };

    group.spawn(increment_after_event(event, completed));
    group.spawn(increment_after_event(event, completed));
    group.spawn(increment_after_event(event, completed));

    event.set();
    EXPECT_EQ(completed, 3);
    event.set();
    EXPECT_EQ(completed, 3);
    loop.run(group.join());
}

TEST(CmpOneShotEventTest, RegistrationRacePublishesWithoutLostWakeups) {
    constexpr int ITERATIONS { 1'000 };
    RunLoop loop {};

    for (int iteration { 0 }; iteration < ITERATIONS; ++iteration) {
        int published { 0 };
        int observed { 0 };
        OneShotEvent event {};
        TaskGroup group {};

        std::jthread setter { [&event, &published] {
            published = 42;
            event.set();
        } };
        group.spawn(observe_published_value(
            event,
            published,
            observed));

        setter.join();
        loop.run(group.join());
        ASSERT_EQ(observed, 42);
    }
}

TEST(CmpOneShotEventTest, WaiterCanReturnToItsRunLoopExplicitly) {
    RunLoop loop {};
    const auto loopThread = std::this_thread::get_id();

    const auto threads = loop.run(run_scheduler_return(
        loop.get_scheduler()));

    EXPECT_NE(threads[0], loopThread);
    EXPECT_EQ(threads[1], loopThread);
}

TEST(CmpOneShotEventTest, ManyWaitersDoNotGrowTheNativeStack) {
    constexpr int WAITER_COUNT { 50'000 };
    RunLoop loop {};

    EXPECT_EQ(loop.run(run_many_waiters(WAITER_COUNT)), WAITER_COUNT);
}

}  // namespace
