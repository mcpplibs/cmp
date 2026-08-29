#include <gtest/gtest.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::ThreadPool;
using mcpplibs::cmp::run_blocking;

using BlockingScheduler = ThreadPool::Scheduler;
using ReturnScheduler = RunLoop::Scheduler;

using namespace std::chrono_literals;

struct ThreadObservation final {
    int value_ {};
    std::thread::id workerThread_ {};
    std::thread::id resumedThread_ {};
};

struct CancellationObservation final {
    bool prerequisiteReached_ { false };
    bool stopRequested_ { false };
    bool cancelled_ { false };
    int invocations_ {};
    std::optional<int> result_ {};
    std::thread::id resumedThread_ {};
};

struct RaceObservation final {
    int completed_ {};
    int cancelled_ {};
    int invalid_ {};
};

struct IsolationObservation final {
    bool operationStarted_ { false };
    bool loopProgressedWhileBlocked_ { false };
    bool operationCompleted_ { false };
};

class MoveOnlyOperation final {
private:
    std::unique_ptr<int> value_ {};
    std::atomic<int>* invocations_ {};
    std::thread::id* workerThread_ {};

public:
    MoveOnlyOperation(
        std::unique_ptr<int> value,
        std::atomic<int>& invocations,
        std::thread::id& workerThread) noexcept
        : value_ { std::move(value) },
          invocations_ { &invocations },
          workerThread_ { &workerThread } {}

    MoveOnlyOperation(const MoveOnlyOperation&) = delete;
    MoveOnlyOperation& operator=(const MoveOnlyOperation&) = delete;
    MoveOnlyOperation(MoveOnlyOperation&&) noexcept = default;
    MoveOnlyOperation& operator=(MoveOnlyOperation&&) noexcept = default;
    ~MoveOnlyOperation() = default;

    std::unique_ptr<int> operator()() & = delete;

    std::unique_ptr<int> operator()() && {
        invocations_->fetch_add(1, std::memory_order_relaxed);
        *workerThread_ = std::this_thread::get_id();
        ++*value_;
        return std::move(value_);
    }
};

static_assert(std::move_constructible<MoveOnlyOperation>);
static_assert(!std::copy_constructible<MoveOnlyOperation>);
static_assert(std::invocable<MoveOnlyOperation>);
static_assert(!std::invocable<MoveOnlyOperation&>);

using MoveOnlyOffload = decltype(run_blocking(
    std::declval<BlockingScheduler>(),
    std::declval<ReturnScheduler>(),
    std::declval<MoveOnlyOperation>()));

static_assert(std::same_as<MoveOnlyOffload, Task<std::unique_ptr<int>>>);

Task<ThreadObservation> observe_move_only(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo,
    std::atomic<int>& invocations) {
    std::thread::id workerThread {};
    auto result = co_await run_blocking(
        blockingWorkers,
        returnTo,
        MoveOnlyOperation {
            std::make_unique<int>(41),
            invocations,
            workerThread
        });

    co_return ThreadObservation {
        *result,
        workerThread,
        std::this_thread::get_id()
    };
}

Task<ThreadObservation> observe_void(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo,
    std::atomic<int>& invocations) {
    std::thread::id workerThread {};

    co_await run_blocking(blockingWorkers, returnTo, [&] {
        invocations.fetch_add(1, std::memory_order_relaxed);
        workerThread = std::this_thread::get_id();
    });

    co_return ThreadObservation {
        invocations.load(std::memory_order_relaxed),
        workerThread,
        std::this_thread::get_id()
    };
}

Task<ThreadObservation> observe_other_pool(
    BlockingScheduler blockingWorkers,
    BlockingScheduler returnTo) {
    std::thread::id workerThread {};
    const auto value = co_await run_blocking(
        blockingWorkers,
        returnTo,
        [&] {
            workerThread = std::this_thread::get_id();
            return 42;
        });

    co_return ThreadObservation {
        value,
        workerThread,
        std::this_thread::get_id()
    };
}

Task<ThreadObservation> observe_exception(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo) {
    try {
        static_cast<void>(co_await run_blocking(
            blockingWorkers,
            returnTo,
            []() -> int {
                throw std::runtime_error { "blocking failure" };
            }));
    } catch (const std::runtime_error& error) {
        co_return ThreadObservation {
            error.what() == std::string_view { "blocking failure" } ? 42 : 0,
            {},
            std::this_thread::get_id()
        };
    }

    co_return ThreadObservation {};
}

Task<CancellationObservation> observe_cancellation(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo,
    std::stop_token stopToken,
    std::atomic<int>& invocations) {
    bool cancelled { false };

    try {
        co_await run_blocking(
            blockingWorkers,
            returnTo,
            [&] {
                invocations.fetch_add(1, std::memory_order_relaxed);
            },
            stopToken);
    } catch (const OperationCancelled&) {
        cancelled = true;
    }

    co_return CancellationObservation {
        true,
        stopToken.stop_requested(),
        cancelled,
        invocations.load(std::memory_order_relaxed),
        {},
        std::this_thread::get_id()
    };
}

Task<void> block_worker(
    BlockingScheduler scheduler,
    std::counting_semaphore<1>& entered,
    std::latch& release) {
    co_await scheduler.schedule();
    entered.release();
    release.wait();
}

Task<void> capture_cancellation(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo,
    std::stop_token stopToken,
    std::atomic<int>& invocations,
    bool& cancelled,
    std::thread::id& resumedThread) {
    try {
        co_await run_blocking(
            blockingWorkers,
            returnTo,
            [&] {
                invocations.fetch_add(1, std::memory_order_relaxed);
            },
            stopToken);
    } catch (const OperationCancelled&) {
        cancelled = true;
    }

    resumedThread = std::this_thread::get_id();
}

Task<CancellationObservation> run_queued_cancellation(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo) {
    std::counting_semaphore<1> entered { 0 };
    std::latch release { 1 };
    std::stop_source stopSource {};
    std::atomic<int> invocations {};
    bool cancelled { false };
    std::thread::id resumedThread {};
    TaskGroup group {};

    group.spawn(block_worker(blockingWorkers, entered, release));
    const bool blockerEntered = entered.try_acquire_for(2s);

    // eager spawn 返回时，offload 已经排在被阻塞 worker 后面。
    group.spawn(capture_cancellation(
        blockingWorkers,
        returnTo,
        stopSource.get_token(),
        invocations,
        cancelled,
        resumedThread));
    const bool stopRequested = stopSource.request_stop();
    release.count_down();

    co_await group.join();
    co_return CancellationObservation {
        blockerEntered,
        stopRequested,
        cancelled,
        invocations.load(std::memory_order_relaxed),
        {},
        resumedThread
    };
}

Task<void> capture_late_result(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo,
    std::stop_token stopToken,
    std::counting_semaphore<1>& entered,
    std::latch& release,
    std::atomic<int>& invocations,
    std::optional<int>& result,
    bool& cancelled,
    std::thread::id& resumedThread) {
    try {
        result.emplace(co_await run_blocking(
            blockingWorkers,
            returnTo,
            [&] {
                invocations.fetch_add(1, std::memory_order_relaxed);
                entered.release();
                release.wait();
                return 42;
            },
            stopToken));
    } catch (const OperationCancelled&) {
        cancelled = true;
    }

    resumedThread = std::this_thread::get_id();
}

Task<CancellationObservation> run_late_cancellation(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo) {
    std::counting_semaphore<1> entered { 0 };
    std::latch release { 1 };
    std::stop_source stopSource {};
    std::atomic<int> invocations {};
    std::optional<int> result {};
    bool cancelled { false };
    std::thread::id resumedThread {};
    TaskGroup group {};

    group.spawn(capture_late_result(
        blockingWorkers,
        returnTo,
        stopSource.get_token(),
        entered,
        release,
        invocations,
        result,
        cancelled,
        resumedThread));

    const bool operationEntered = entered.try_acquire_for(2s);
    const bool stopRequested = stopSource.request_stop();
    release.count_down();

    co_await group.join();
    co_return CancellationObservation {
        operationEntered,
        stopRequested,
        cancelled,
        invocations.load(std::memory_order_relaxed),
        result,
        resumedThread
    };
}

Task<RaceObservation> run_cancellation_races(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo,
    std::thread::id expectedThread,
    int raceCount) {
    RaceObservation observation {};

    for (int index { 0 }; index < raceCount; ++index) {
        std::stop_source stopSource {};
        std::atomic<int> invocations {};
        std::jthread canceller { [&stopSource] {
            stopSource.request_stop();
        } };

        try {
            const auto result = co_await run_blocking(
                blockingWorkers,
                returnTo,
                [&] {
                    invocations.fetch_add(1, std::memory_order_relaxed);
                    return 42;
                },
                stopSource.get_token());
            ++observation.completed_;

            if (result != 42 ||
                invocations.load(std::memory_order_relaxed) != 1) {
                ++observation.invalid_;
            }
        } catch (const OperationCancelled&) {
            ++observation.cancelled_;

            if (invocations.load(std::memory_order_relaxed) != 0) {
                ++observation.invalid_;
            }
        } catch (...) {
            ++observation.invalid_;
        }

        canceller.join();

        if (std::this_thread::get_id() != expectedThread) {
            ++observation.invalid_;
        }
    }

    co_return observation;
}

BlockingScheduler make_expired_scheduler() {
    ThreadPool pool { 1 };
    return pool.get_scheduler();
}

Task<void> mark_run_loop_ready(
    ReturnScheduler scheduler,
    bool& progressed) {
    co_await scheduler.schedule();
    progressed = true;
}

Task<IsolationObservation> observe_run_loop_progress(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo) {
    std::counting_semaphore<1> entered { 0 };
    std::latch release { 1 };
    std::atomic<bool> completed { false };
    bool progressed { false };
    TaskGroup group {};

    group.spawn(run_blocking(blockingWorkers, returnTo, [&] {
        entered.release();
        release.wait();
        completed.store(true, std::memory_order_release);
    }));

    const bool operationStarted = entered.try_acquire_for(2s);
    group.spawn(mark_run_loop_ready(returnTo, progressed));

    // marker 先于当前 continuation 入队，证明 RunLoop 在阻塞调用期间仍能推进。
    co_await returnTo.schedule();
    const bool progressedWhileBlocked = progressed &&
        !completed.load(std::memory_order_acquire);
    release.count_down();

    co_await group.join();
    co_return IsolationObservation {
        operationStarted,
        progressedWhileBlocked,
        completed.load(std::memory_order_acquire)
    };
}

Task<void> increment_offloaded(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo,
    std::atomic<int>& invoked,
    int& returned) {
    co_await run_blocking(blockingWorkers, returnTo, [&] {
        invoked.fetch_add(1, std::memory_order_relaxed);
    });
    ++returned;
}

Task<std::pair<int, int>> run_many_offloads(
    BlockingScheduler blockingWorkers,
    ReturnScheduler returnTo,
    int operationCount) {
    std::atomic<int> invoked {};
    int returned { 0 };
    TaskGroup group {};

    for (int index { 0 }; index < operationCount; ++index) {
        group.spawn(increment_offloaded(
            blockingWorkers,
            returnTo,
            invoked,
            returned));
    }

    co_await group.join();
    co_return std::pair {
        invoked.load(std::memory_order_relaxed),
        returned
    };
}

TEST(CmpBlockingTest, IsLazyAndReturnsMoveOnlyValueOnRequestedRunLoop) {
    ThreadPool blockingWorkers { 1 };
    RunLoop loop {};
    std::atomic<int> invocations {};
    const auto callerThread = std::this_thread::get_id();
    auto task = observe_move_only(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler(),
        invocations);

    EXPECT_EQ(invocations.load(std::memory_order_relaxed), 0);
    const auto observation = loop.run(std::move(task));

    EXPECT_EQ(observation.value_, 42);
    EXPECT_EQ(invocations.load(std::memory_order_relaxed), 1);
    EXPECT_NE(observation.workerThread_, callerThread);
    EXPECT_EQ(observation.resumedThread_, callerThread);
}

TEST(CmpBlockingTest, SupportsVoidAndReturnsToRequestedRunLoop) {
    ThreadPool blockingWorkers { 1 };
    RunLoop loop {};
    std::atomic<int> invocations {};
    const auto callerThread = std::this_thread::get_id();
    const auto observation = loop.run(observe_void(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler(),
        invocations));

    EXPECT_EQ(observation.value_, 1);
    EXPECT_NE(observation.workerThread_, callerThread);
    EXPECT_EQ(observation.resumedThread_, callerThread);
}

TEST(CmpBlockingTest, CanReturnToAnotherThreadPool) {
    ThreadPool blockingWorkers { 1 };
    ThreadPool returnWorkers { 1 };
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    const auto observation = loop.run(observe_other_pool(
        blockingWorkers.get_scheduler(),
        returnWorkers.get_scheduler()));

    EXPECT_EQ(observation.value_, 42);
    EXPECT_NE(observation.workerThread_, callerThread);
    EXPECT_NE(observation.resumedThread_, callerThread);
    EXPECT_NE(observation.workerThread_, observation.resumedThread_);
}

TEST(CmpBlockingTest, TransportsCallableExceptionOnReturnScheduler) {
    ThreadPool blockingWorkers { 1 };
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    const auto observation = loop.run(observe_exception(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler()));

    EXPECT_EQ(observation.value_, 42);
    EXPECT_EQ(observation.resumedThread_, callerThread);
}

TEST(CmpBlockingTest, PreCancellationSkipsCallableAndReturnsBeforeThrowing) {
    ThreadPool blockingWorkers { 1 };
    RunLoop loop {};
    std::stop_source stopSource {};
    std::atomic<int> invocations {};
    const auto callerThread = std::this_thread::get_id();
    stopSource.request_stop();

    const auto observation = loop.run(observe_cancellation(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler(),
        stopSource.get_token(),
        invocations));

    EXPECT_TRUE(observation.cancelled_);
    EXPECT_EQ(observation.invocations_, 0);
    EXPECT_EQ(observation.resumedThread_, callerThread);
}

TEST(CmpBlockingTest, QueuedCancellationSkipsCallable) {
    ThreadPool blockingWorkers { 1 };
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    const auto observation = loop.run(run_queued_cancellation(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler()));

    EXPECT_TRUE(observation.prerequisiteReached_);
    EXPECT_TRUE(observation.stopRequested_);
    EXPECT_TRUE(observation.cancelled_);
    EXPECT_EQ(observation.invocations_, 0);
    EXPECT_EQ(observation.resumedThread_, callerThread);
}

TEST(CmpBlockingTest, LateCancellationPreservesClaimedResult) {
    ThreadPool blockingWorkers { 1 };
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    const auto observation = loop.run(run_late_cancellation(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler()));

    EXPECT_TRUE(observation.prerequisiteReached_);
    EXPECT_TRUE(observation.stopRequested_);
    EXPECT_FALSE(observation.cancelled_);
    ASSERT_TRUE(observation.result_.has_value());
    EXPECT_EQ(*observation.result_, 42);
    EXPECT_EQ(observation.invocations_, 1);
    EXPECT_EQ(observation.resumedThread_, callerThread);
}

TEST(CmpBlockingTest, CancellationRaceInvokesAtMostOnce) {
    constexpr int RACE_COUNT { 500 };
    ThreadPool blockingWorkers { 2 };
    RunLoop loop {};
    const auto observation = loop.run(run_cancellation_races(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler(),
        std::this_thread::get_id(),
        RACE_COUNT));

    EXPECT_EQ(observation.completed_ + observation.cancelled_, RACE_COUNT);
    EXPECT_EQ(observation.invalid_, 0);
}

TEST(CmpBlockingTest, RejectsExpiredWorkerAndReturnSchedulers) {
    ThreadPool blockingWorkers { 1 };
    RunLoop loop {};
    RunLoop inactiveLoop {};
    std::atomic<int> invocations {};
    const auto expiredWorker = make_expired_scheduler();
    const auto expiredReturn = make_expired_scheduler();

    auto rejectedWorker = run_blocking(
        expiredWorker,
        loop.get_scheduler(),
        [&] {
            invocations.fetch_add(1, std::memory_order_relaxed);
        });
    EXPECT_THROW(loop.run(std::move(rejectedWorker)), std::logic_error);
    EXPECT_EQ(invocations.load(std::memory_order_relaxed), 0);

    auto rejectedReturn = run_blocking(
        blockingWorkers.get_scheduler(),
        expiredReturn,
        [&] {
            invocations.fetch_add(1, std::memory_order_relaxed);
        });
    EXPECT_THROW(loop.run(std::move(rejectedReturn)), std::logic_error);
    EXPECT_EQ(invocations.load(std::memory_order_relaxed), 1);

    auto inactiveReturn = run_blocking(
        blockingWorkers.get_scheduler(),
        inactiveLoop.get_scheduler(),
        [&] {
            invocations.fetch_add(1, std::memory_order_relaxed);
        });
    EXPECT_THROW(loop.run(std::move(inactiveReturn)), std::logic_error);
    EXPECT_EQ(invocations.load(std::memory_order_relaxed), 2);
}

TEST(CmpBlockingTest, KeepsRunLoopResponsiveWhileCallableBlocks) {
    ThreadPool blockingWorkers { 1 };
    RunLoop loop {};
    const auto observation = loop.run(observe_run_loop_progress(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler()));

    EXPECT_TRUE(observation.operationStarted_);
    EXPECT_TRUE(observation.loopProgressedWhileBlocked_);
    EXPECT_TRUE(observation.operationCompleted_);
}

TEST(CmpBlockingTest, ManyConcurrentOffloadsCompleteExactlyOnce) {
    constexpr int OPERATION_COUNT { 5'000 };
    ThreadPool blockingWorkers { 4 };
    RunLoop loop {};
    const auto [invoked, returned] = loop.run(run_many_offloads(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler(),
        OPERATION_COUNT));

    EXPECT_EQ(invoked, OPERATION_COUNT);
    EXPECT_EQ(returned, OPERATION_COUNT);
}

}  // namespace
