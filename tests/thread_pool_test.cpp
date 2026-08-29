#include <gtest/gtest.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::ThreadPool;
using mcpplibs::cmp::when_all;

using Scheduler = ThreadPool::Scheduler;

using namespace std::chrono_literals;

static_assert(std::default_initializable<ThreadPool>);
static_assert(!std::copy_constructible<ThreadPool>);
static_assert(!std::move_constructible<ThreadPool>);
static_assert(!std::is_copy_assignable_v<ThreadPool>);
static_assert(!std::is_move_assignable_v<ThreadPool>);

static_assert(!std::default_initializable<Scheduler>);
static_assert(std::copy_constructible<Scheduler>);
static_assert(std::move_constructible<Scheduler>);
static_assert(std::equality_comparable<Scheduler>);

struct CancellationRecord final {
    bool cancelled_ { false };
    std::thread::id thread_ {};
};

Task<std::thread::id> schedule_once(Scheduler scheduler) {
    co_await scheduler.schedule();
    co_return std::this_thread::get_id();
}

Task<void> schedule_uncaught(Scheduler scheduler) {
    co_await scheduler.schedule();
}

Task<int> value_on_pool(Scheduler scheduler, int value) {
    co_await scheduler.schedule();
    co_return value;
}

Task<int> fail_on_pool(Scheduler scheduler) {
    co_await scheduler.schedule();
    throw std::runtime_error { "pool failure" };
    co_return 0;
}

Task<std::pair<std::thread::id, std::thread::id>> return_to_run_loop(
    Scheduler workers,
    RunLoop::Scheduler caller) {
    co_await workers.schedule();
    const auto workerThread = std::this_thread::get_id();
    co_await caller.schedule();
    co_return std::pair { workerThread, std::this_thread::get_id() };
}

Task<void> block_worker(
    Scheduler scheduler,
    std::counting_semaphore<1>& entered,
    std::latch& release) {
    co_await scheduler.schedule();
    entered.release();
    release.wait();
}

Task<void> record_on_pool(
    Scheduler scheduler,
    int value,
    std::vector<int>& order) {
    co_await scheduler.schedule();
    order.push_back(value);
}

Task<void> occupy_worker(
    Scheduler scheduler,
    std::counting_semaphore<2>& entered,
    std::latch& release,
    std::mutex& threadsMutex,
    std::set<std::thread::id>& threads) {
    co_await scheduler.schedule();

    {
        const std::lock_guard lock { threadsMutex };
        threads.insert(std::this_thread::get_id());
    }

    entered.release();
    release.wait();
}

Task<void> run_nested_fanout(
    Scheduler scheduler,
    std::counting_semaphore<2>& entered,
    std::latch& release,
    std::mutex& threadsMutex,
    std::set<std::thread::id>& threads) {
    co_await scheduler.schedule();

    TaskGroup group {};
    group.spawn(occupy_worker(
        scheduler,
        entered,
        release,
        threadsMutex,
        threads));
    group.spawn(occupy_worker(
        scheduler,
        entered,
        release,
        threadsMutex,
        threads));
    co_await group.join();
}

Task<CancellationRecord> schedule_once(
    Scheduler scheduler,
    std::stop_token stopToken) {
    try {
        co_await scheduler.schedule(stopToken);
        co_return CancellationRecord {
            false,
            std::this_thread::get_id()
        };
    } catch (const OperationCancelled&) {
        co_return CancellationRecord {
            true,
            std::this_thread::get_id()
        };
    }
}

Task<void> observe_cancellation(
    Scheduler scheduler,
    std::stop_token stopToken,
    bool& cancelled,
    std::thread::id& resumedThread) {
    try {
        co_await scheduler.schedule(stopToken);
    } catch (const OperationCancelled&) {
        cancelled = true;
    }

    resumedThread = std::this_thread::get_id();
}

Task<bool> request_stop_after_claim(
    Scheduler scheduler,
    std::stop_source& stopSource) {
    co_await scheduler.schedule(stopSource.get_token());
    co_return stopSource.request_stop();
}

Task<bool> race_cancellation(
    Scheduler scheduler,
    std::stop_token stopToken,
    int& resumeCount) {
    bool cancelled { false };

    try {
        co_await scheduler.schedule(stopToken);
    } catch (const OperationCancelled&) {
        cancelled = true;
    }

    ++resumeCount;
    co_return cancelled;
}

Task<int> schedule_pre_cancelled_many_times(
    Scheduler scheduler,
    std::stop_token stopToken,
    int count) {
    int cancellationCount { 0 };

    for (int index { 0 }; index < count; ++index) {
        try {
            co_await scheduler.schedule(stopToken);
        } catch (const OperationCancelled&) {
            ++cancellationCount;
        }
    }

    co_return cancellationCount;
}

Task<void> reschedule_many_times(
    Scheduler scheduler,
    int count,
    std::atomic<int>& completed) {
    for (int index { 0 }; index < count; ++index) {
        co_await scheduler.schedule();
        completed.fetch_add(1, std::memory_order_relaxed);
    }
}

Task<int> run_concurrent_producers(
    Scheduler scheduler,
    int producerCount,
    int schedulesPerProducer) {
    std::atomic<int> completed {};
    TaskGroup group {};

    for (int index { 0 }; index < producerCount; ++index) {
        group.spawn(reschedule_many_times(
            scheduler,
            schedulesPerProducer,
            completed));
    }

    co_await group.join();
    co_return completed.load(std::memory_order_relaxed);
}

Task<void> increment_once(
    Scheduler scheduler,
    std::atomic<int>& completed) {
    co_await scheduler.schedule();
    completed.fetch_add(1, std::memory_order_relaxed);
}

Task<bool> try_schedule_during_close(
    Scheduler scheduler,
    std::atomic<int>& resumeCount) {
    bool accepted { false };

    try {
        co_await scheduler.schedule();
        accepted = true;
    } catch (const std::logic_error&) {
    }

    resumeCount.fetch_add(1, std::memory_order_relaxed);
    co_return accepted;
}

Scheduler make_expired_scheduler() {
    ThreadPool pool { 1 };
    return pool.get_scheduler();
}

std::pair<Scheduler, Scheduler> make_same_expired_schedulers() {
    ThreadPool pool { 1 };
    return {
        pool.get_scheduler(),
        pool.get_scheduler()
    };
}

TEST(CmpThreadPoolTest, ReportsConstructionAndSchedulerIdentity) {
    const auto expectedDefaultThreadCount = std::max<std::size_t>(
        1,
        std::thread::hardware_concurrency());
    ThreadPool defaultPool {};
    ThreadPool first { 2 };
    ThreadPool second { 1 };

    EXPECT_EQ(defaultPool.thread_count(), expectedDefaultThreadCount);
    EXPECT_EQ(first.thread_count(), 2U);
    EXPECT_EQ(second.thread_count(), 1U);
    EXPECT_EQ(first.get_scheduler(), first.get_scheduler());
    EXPECT_NE(first.get_scheduler(), second.get_scheduler());
    EXPECT_THROW(ThreadPool { 0 }, std::invalid_argument);
}

TEST(CmpThreadPoolTest, SchedulingAlwaysSuspendsAndUsesAWorker) {
    ThreadPool pool { 1 };
    RunLoop loop {};
    const auto scheduler = pool.get_scheduler();
    const auto callerThread = std::this_thread::get_id();

    EXPECT_FALSE(scheduler.schedule().await_ready());
    EXPECT_NE(loop.run(schedule_once(scheduler)), callerThread);
}

TEST(CmpThreadPoolTest, ReturnsExplicitlyToTheRunLoopThread) {
    ThreadPool pool { 2 };
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    const auto [workerThread, returnedThread] = loop.run(return_to_run_loop(
        pool.get_scheduler(),
        loop.get_scheduler()));

    EXPECT_NE(workerThread, callerThread);
    EXPECT_EQ(returnedThread, callerThread);
}

TEST(CmpThreadPoolTest, PreservesOneWorkerFifoDequeueOrder) {
    constexpr int ENTRY_COUNT { 100 };
    ThreadPool pool { 1 };
    const auto scheduler = pool.get_scheduler();
    std::counting_semaphore<1> entered { 0 };
    std::latch release { 1 };
    std::vector<int> order {};
    TaskGroup group {};
    order.reserve(ENTRY_COUNT);

    group.spawn(block_worker(scheduler, entered, release));
    const bool blockerEntered = entered.try_acquire_for(2s);

    for (int value { 0 }; value < ENTRY_COUNT; ++value) {
        group.spawn(record_on_pool(scheduler, value, order));
    }

    release.count_down();
    RunLoop loop {};
    loop.run(group.join());

    EXPECT_TRUE(blockerEntered);
    EXPECT_EQ(order.size(), static_cast<std::size_t>(ENTRY_COUNT));
    EXPECT_TRUE(std::ranges::equal(
        order,
        std::views::iota(0, ENTRY_COUNT)));
}

TEST(CmpThreadPoolTest, WorkerOriginFanoutWakesASecondWorker) {
    ThreadPool pool { 2 };
    RunLoop loop {};
    std::counting_semaphore<2> entered { 0 };
    std::latch release { 1 };
    std::mutex threadsMutex {};
    std::set<std::thread::id> threads {};
    std::exception_ptr driverException {};

    std::jthread driver { [&] {
        try {
            loop.run(run_nested_fanout(
                pool.get_scheduler(),
                entered,
                release,
                threadsMutex,
                threads));
        } catch (...) {
            driverException = std::current_exception();
        }
    } };

    const bool firstEntered = entered.try_acquire_for(2s);
    const bool secondEntered = entered.try_acquire_for(2s);
    release.count_down();
    driver.join();

    EXPECT_TRUE(firstEntered);
    EXPECT_TRUE(secondEntered);
    EXPECT_FALSE(driverException);
    EXPECT_EQ(threads.size(), 2U);
}

TEST(CmpThreadPoolTest, IntegratesValuesExceptionsWhenAllAndTaskGroup) {
    ThreadPool pool { 4 };
    RunLoop loop {};
    const auto scheduler = pool.get_scheduler();

    EXPECT_EQ(loop.run(value_on_pool(scheduler, 42)), 42);
    EXPECT_THROW(loop.run(fail_on_pool(scheduler)), std::runtime_error);

    const auto [first, second] = loop.run(when_all(
        value_on_pool(scheduler, 20),
        value_on_pool(scheduler, 22)));
    EXPECT_EQ(first + second, 42);

    EXPECT_EQ(
        loop.run(run_concurrent_producers(scheduler, 8, 250)),
        2'000);
}

TEST(CmpThreadPoolTest, SupportsNormalAndPreCancelledScheduling) {
    ThreadPool pool { 1 };
    RunLoop loop {};
    std::stop_source stopped {};
    const auto callerThread = std::this_thread::get_id();
    stopped.request_stop();

    const auto normal = loop.run(schedule_once(
        pool.get_scheduler(),
        std::stop_token {}));
    const auto cancelled = loop.run(schedule_once(
        pool.get_scheduler(),
        stopped.get_token()));

    EXPECT_FALSE(normal.cancelled_);
    EXPECT_TRUE(cancelled.cancelled_);
    EXPECT_NE(normal.thread_, callerThread);
    EXPECT_NE(cancelled.thread_, callerThread);
}

TEST(CmpThreadPoolTest, CancelsAnEntryQueuedBehindAWorker) {
    ThreadPool pool { 1 };
    const auto scheduler = pool.get_scheduler();
    std::counting_semaphore<1> entered { 0 };
    std::latch release { 1 };
    std::stop_source stopSource {};
    bool cancelled { false };
    std::thread::id resumedThread {};
    TaskGroup group {};

    group.spawn(block_worker(scheduler, entered, release));
    const bool blockerEntered = entered.try_acquire_for(2s);

    // spawn 返回时，取消项已经确定排在被阻塞 worker 后面。
    group.spawn(observe_cancellation(
        scheduler,
        stopSource.get_token(),
        cancelled,
        resumedThread));
    const bool requestWon = stopSource.request_stop();
    release.count_down();

    RunLoop loop {};
    loop.run(group.join());

    EXPECT_TRUE(blockerEntered);
    EXPECT_TRUE(requestWon);
    EXPECT_TRUE(cancelled);
    EXPECT_NE(resumedThread, std::thread::id {});
}

TEST(CmpThreadPoolTest, LateStopDoesNotReplaceAClaimedEntry) {
    ThreadPool pool { 1 };
    RunLoop loop {};
    std::stop_source stopSource {};

    EXPECT_TRUE(loop.run(request_stop_after_claim(
        pool.get_scheduler(),
        stopSource)));
    EXPECT_TRUE(stopSource.stop_requested());
}

TEST(CmpThreadPoolTest, CancellationRaceResumesExactlyOnce) {
    constexpr int RACE_COUNT { 500 };
    ThreadPool pool { 2 };
    RunLoop loop {};
    int completedCount { 0 };
    int cancelledCount { 0 };

    for (int index { 0 }; index < RACE_COUNT; ++index) {
        std::stop_source stopSource {};
        int resumeCount { 0 };
        std::jthread canceller { [&stopSource] {
            stopSource.request_stop();
        } };

        if (loop.run(race_cancellation(
                pool.get_scheduler(),
                stopSource.get_token(),
                resumeCount))) {
            ++cancelledCount;
        } else {
            ++completedCount;
        }

        canceller.join();
        EXPECT_EQ(resumeCount, 1);
    }

    EXPECT_EQ(completedCount + cancelledCount, RACE_COUNT);
}

TEST(CmpThreadPoolTest, ConcurrentReschedulingIsExactlyOnceAndStackSafe) {
    constexpr int PRODUCER_COUNT { 8 };
    constexpr int SCHEDULES_PER_PRODUCER { 2'000 };
    ThreadPool pool { 4 };
    RunLoop loop {};

    EXPECT_EQ(
        loop.run(run_concurrent_producers(
            pool.get_scheduler(),
            PRODUCER_COUNT,
            SCHEDULES_PER_PRODUCER)),
        PRODUCER_COUNT * SCHEDULES_PER_PRODUCER);
}

TEST(CmpThreadPoolTest, RepeatedPreCancellationDoesNotGrowTheStack) {
    constexpr int SCHEDULE_COUNT { 20'000 };
    ThreadPool pool { 2 };
    RunLoop loop {};
    std::stop_source stopSource {};
    stopSource.request_stop();

    EXPECT_EQ(
        loop.run(schedule_pre_cancelled_many_times(
            pool.get_scheduler(),
            stopSource.get_token(),
            SCHEDULE_COUNT)),
        SCHEDULE_COUNT);
}

TEST(CmpThreadPoolTest, DestructionDrainsAcceptedEntries) {
    constexpr int ENTRY_COUNT { 5'000 };
    auto pool = std::make_unique<ThreadPool>(2);
    const auto scheduler = pool->get_scheduler();
    std::atomic<int> completed {};
    TaskGroup group {};

    for (int index { 0 }; index < ENTRY_COUNT; ++index) {
        group.spawn(increment_once(scheduler, completed));
    }

    pool.reset();
    RunLoop loop {};
    loop.run(group.join());

    EXPECT_EQ(completed.load(std::memory_order_relaxed), ENTRY_COUNT);
    EXPECT_THROW(loop.run(schedule_uncaught(scheduler)), std::logic_error);
}

TEST(CmpThreadPoolTest, ConcurrentCloseEitherAcceptsOrRejectsWithoutLoss) {
    auto pool = std::make_unique<ThreadPool>(1);
    const auto scheduler = pool->get_scheduler();
    std::counting_semaphore<1> entered { 0 };
    std::latch release { 1 };
    std::barrier start { 3 };
    std::atomic<int> resumeCount {};
    std::optional<bool> accepted {};
    std::exception_ptr submitException {};
    TaskGroup blocker {};
    RunLoop submitLoop {};

    blocker.spawn(block_worker(scheduler, entered, release));
    const bool blockerEntered = entered.try_acquire_for(2s);

    std::jthread destroyer { [&] {
        start.arrive_and_wait();
        pool.reset();
    } };
    std::jthread submitter { [&] {
        start.arrive_and_wait();
        try {
            accepted.emplace(submitLoop.run(try_schedule_during_close(
                scheduler,
                resumeCount)));
        } catch (...) {
            submitException = std::current_exception();
        }
    } };

    start.arrive_and_wait();
    release.count_down();
    destroyer.join();
    submitter.join();

    RunLoop blockerLoop {};
    blockerLoop.run(blocker.join());

    EXPECT_TRUE(blockerEntered);
    EXPECT_FALSE(submitException);
    EXPECT_TRUE(accepted.has_value());
    EXPECT_EQ(resumeCount.load(std::memory_order_relaxed), 1);
}

TEST(CmpThreadPoolTest, PreservesExpiredIdentityAndRejectsScheduling) {
    RunLoop loop {};
    std::stop_source stopSource {};
    const auto [expired, sameExpired] = make_same_expired_schedulers();
    const auto otherExpired = make_expired_scheduler();
    stopSource.request_stop();

    EXPECT_EQ(expired, sameExpired);
    EXPECT_NE(expired, otherExpired);
    EXPECT_THROW(loop.run(schedule_uncaught(expired)), std::logic_error);
    EXPECT_THROW(
        loop.run(schedule_once(expired, stopSource.get_token())),
        std::logic_error);
}

}  // namespace
