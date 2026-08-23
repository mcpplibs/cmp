#include <gtest/gtest.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::AsyncMutex;
using mcpplibs::cmp::OneShotEvent;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;

using Scheduler = RunLoop::Scheduler;

Task<int> read_under_lock(AsyncMutex& mutex, int value) {
    auto guard = co_await mutex.lock_async();
    co_return value;
}

Task<int> read_with_moved_guard(AsyncMutex& mutex, int value) {
    auto guard = co_await mutex.lock_async();
    auto movedGuard = std::move(guard);
    co_return value;
}

Task<void> fail_under_lock(AsyncMutex& mutex) {
    auto guard = co_await mutex.lock_async();
    throw std::runtime_error { "locked failure" };
}

Task<void> scheduled_holder(
    AsyncMutex& mutex,
    Scheduler scheduler,
    std::vector<int>& events) {
    auto guard = co_await mutex.lock_async();
    events.push_back(1);
    co_await scheduler.schedule();
    events.push_back(4);
    co_return;
}

Task<void> record_after_lock(
    AsyncMutex& mutex,
    int value,
    std::vector<int>& events) {
    auto guard = co_await mutex.lock_async();
    events.push_back(value);
    co_return;
}

Task<std::vector<int>> run_fifo_handoff(
    Scheduler scheduler,
    bool& waitersSuspended) {
    std::vector<int> events {};
    AsyncMutex mutex {};
    TaskGroup group {};

    group.spawn(scheduled_holder(mutex, scheduler, events));
    group.spawn(record_after_lock(mutex, 2, events));
    group.spawn(record_after_lock(mutex, 3, events));
    waitersSuspended = events == std::vector<int> { 1 };

    co_await group.join();
    co_return events;
}

Task<void> release_on_event(
    AsyncMutex& mutex,
    OneShotEvent& release,
    std::thread::id& releaseThread) {
    auto guard = co_await mutex.lock_async();
    co_await release;
    releaseThread = std::this_thread::get_id();
    co_return;
}

Task<void> record_acquisition_thread(
    AsyncMutex& mutex,
    std::thread::id& acquisitionThread) {
    auto guard = co_await mutex.lock_async();
    acquisitionThread = std::this_thread::get_id();
    co_return;
}

Task<std::array<std::thread::id, 2>> run_cross_thread_handoff(
    Scheduler scheduler,
    std::jthread& setter) {
    std::array<std::thread::id, 2> threads {};
    OneShotEvent release {};
    AsyncMutex mutex {};
    TaskGroup group {};

    group.spawn(release_on_event(mutex, release, threads[0]));
    group.spawn(record_acquisition_thread(mutex, threads[1]));
    auto* const setterPointer = &setter;
    *setterPointer = std::jthread { [&release] {
        release.set();
    } };

    co_await group.join();
    co_await scheduler.schedule();
    setter.join();
    co_return threads;
}

Task<void> hold_until_set(
    AsyncMutex& mutex,
    OneShotEvent& release) {
    auto guard = co_await mutex.lock_async();
    co_await release;
    co_return;
}

Task<void> increment_under_lock(
    AsyncMutex& mutex,
    int& completed) {
    auto guard = co_await mutex.lock_async();
    ++completed;
    co_return;
}

Task<int> run_many_handoffs(int count) {
    int completed { 0 };
    OneShotEvent release {};
    AsyncMutex mutex {};
    TaskGroup group {};

    group.spawn(hold_until_set(mutex, release));
    for (int index { 0 }; index < count; ++index) {
        group.spawn(increment_under_lock(mutex, completed));
    }

    release.set();
    co_await group.join();
    co_return completed;
}

TEST(CmpAsyncMutexTest, IsImmovableAndGuardOwnsThroughMove) {
    static_assert(!std::copy_constructible<AsyncMutex>);
    static_assert(!std::move_constructible<AsyncMutex>);
    static_assert(std::move_constructible<AsyncMutex::Guard>);
    static_assert(!std::copy_constructible<AsyncMutex::Guard>);

    RunLoop loop {};
    AsyncMutex mutex {};
    EXPECT_EQ(loop.run(read_with_moved_guard(mutex, 42)), 42);
    EXPECT_EQ(loop.run(read_under_lock(mutex, 7)), 7);
}

TEST(CmpAsyncMutexTest, GuardReleasesDuringExceptionUnwinding) {
    RunLoop loop {};
    AsyncMutex mutex {};

    EXPECT_THROW(loop.run(fail_under_lock(mutex)), std::runtime_error);
    EXPECT_EQ(loop.run(read_under_lock(mutex, 42)), 42);
}

TEST(CmpAsyncMutexTest, ContentionSuspendsAndHandsOffInFifoOrder) {
    RunLoop loop {};
    bool waitersSuspended { false };

    const auto events = loop.run(run_fifo_handoff(
        loop.get_scheduler(),
        waitersSuspended));

    EXPECT_TRUE(waitersSuspended);
    EXPECT_EQ(events, (std::vector<int> { 1, 4, 2, 3 }));
}

TEST(CmpAsyncMutexTest, OwnershipCanMoveAcrossThreads) {
    RunLoop loop {};
    const auto loopThread = std::this_thread::get_id();
    std::jthread setter {};

    const auto threads = loop.run(run_cross_thread_handoff(
        loop.get_scheduler(),
        setter));

    EXPECT_NE(threads[0], loopThread);
    EXPECT_EQ(threads[1], threads[0]);
}

TEST(CmpAsyncMutexTest, ContendedHandoffsDoNotGrowTheNativeStack) {
    constexpr int HANDOFF_COUNT { 50'000 };
    RunLoop loop {};

    EXPECT_EQ(loop.run(run_many_handoffs(HANDOFF_COUNT)), HANDOFF_COUNT);
}

}  // namespace
