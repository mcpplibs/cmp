#include <gtest/gtest.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;

using Scheduler = RunLoop::Scheduler;

class LifetimeToken final {
private:
    int* liveCount_ {};

public:
    explicit LifetimeToken(int& liveCount) noexcept
        : liveCount_ { &liveCount } {
        ++*liveCount_;
    }

    LifetimeToken(const LifetimeToken&) = delete;
    LifetimeToken& operator=(const LifetimeToken&) = delete;

    LifetimeToken(LifetimeToken&& other) noexcept
        : liveCount_ { std::exchange(other.liveCount_, nullptr) } {}

    LifetimeToken& operator=(LifetimeToken&&) = delete;

    ~LifetimeToken() {
        if (liveCount_) {
            --*liveCount_;
        }
    }
};

Task<void> hold_token(LifetimeToken token) {
    static_cast<void>(token);
    co_return;
}

Task<void> scheduled_step(
    Scheduler scheduler,
    int value,
    std::vector<int>& events) {
    events.push_back(value);
    co_await scheduler.schedule();
    events.push_back(value + 10);
    co_return;
}

Task<std::vector<int>> run_scheduled_group(
    Scheduler scheduler,
    bool& firstStartedInline,
    bool& secondStartedInline) {
    std::vector<int> events {};
    TaskGroup group {};

    group.spawn(scheduled_step(scheduler, 1, events));
    firstStartedInline = events == std::vector<int> { 1 };

    group.spawn(scheduled_step(scheduler, 2, events));
    secondStartedInline = events == std::vector<int> { 1, 2 };

    co_await group.join();
    co_return events;
}

Task<void> join_empty_group() {
    TaskGroup group {};
    co_await group.join();
}

Task<void> mark_started(bool& started) {
    started = true;
    co_return;
}

Task<void> spawn_after_join_starts(
    Scheduler scheduler,
    TaskGroup& group,
    bool& admitted,
    bool& candidateStarted) {
    co_await scheduler.schedule();

    group.spawn(mark_started(candidateStarted));
    admitted = true;

    co_return;
}

Task<void> verify_join_allows_recursive_admission(
    Scheduler scheduler,
    bool& admitted,
    bool& candidateStarted) {
    TaskGroup group {};
    group.spawn(spawn_after_join_starts(
        scheduler,
        group,
        admitted,
        candidateStarted));
    co_await group.join();
}

Task<void> verify_lazy_join(bool& started) {
    TaskGroup group {};
    auto pendingJoin = group.join();
    group.spawn(mark_started(started));
    co_await std::move(pendingJoin);
}

Task<void> increment_atomic(std::atomic<int>& value) {
    value.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

Task<void> fail_after_schedules(
    Scheduler scheduler,
    int scheduleCount,
    std::string message,
    std::vector<std::string>& completions) {
    for (int index { 0 }; index < scheduleCount; ++index) {
        co_await scheduler.schedule();
    }

    completions.push_back(message);
    throw std::runtime_error { message };
}

Task<void> run_failing_group(
    Scheduler scheduler,
    std::vector<std::string>& completions) {
    TaskGroup group {};
    group.spawn(fail_after_schedules(
        scheduler,
        2,
        "first",
        completions));
    group.spawn(fail_after_schedules(
        scheduler,
        1,
        "second",
        completions));
    co_await group.join();
}

Task<void> observe_cancellation(
    Scheduler scheduler,
    std::stop_token token,
    bool& cancelled) {
    try {
        co_await scheduler.schedule_after(std::chrono::hours { 1 }, token);
    } catch (const OperationCancelled&) {
        cancelled = true;
    }

    co_return;
}

Task<void> run_cancelled_group(
    Scheduler scheduler,
    bool& firstCancelled,
    bool& secondCancelled,
    bool& firstRequestWon,
    bool& secondRequestWon) {
    TaskGroup group {};
    const auto token = group.get_stop_token();

    group.spawn(observe_cancellation(
        scheduler,
        token,
        firstCancelled));
    firstRequestWon = group.request_stop();
    group.spawn(observe_cancellation(
        scheduler,
        token,
        secondCancelled));
    secondRequestWon = group.request_stop();

    co_await group.join();
}

class ResumeOnNewThread final {
private:
    std::jthread* worker_ {};

public:
    explicit ResumeOnNewThread(std::jthread& worker) noexcept
        : worker_ { &worker } {}

    [[nodiscard]] constexpr bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) const {
        auto* const worker = worker_;
        *worker = std::jthread { [continuation] {
            continuation.resume();
        } };
    }

    constexpr void await_resume() const noexcept {}
};

Task<void> add_on_new_thread(
    std::jthread& worker,
    std::atomic<int>& total,
    int value) {
    co_await ResumeOnNewThread { worker };
    total.fetch_add(value, std::memory_order_relaxed);
    co_return;
}

Task<void> run_cross_thread_group(
    std::array<std::jthread, 2>& workers,
    std::atomic<int>& total) {
    TaskGroup group {};
    group.spawn(add_on_new_thread(workers[0], total, 3));
    group.spawn(add_on_new_thread(workers[1], total, 4));
    co_await group.join();
}

Task<void> increment(int& value) {
    ++value;
    co_return;
}

Task<int> run_immediate_group(int count) {
    int completed { 0 };
    TaskGroup group {};

    for (int index { 0 }; index < count; ++index) {
        group.spawn(increment(completed));
    }

    co_await group.join();
    co_return completed;
}

Task<void> spawn_recursive_after_schedule(
    Scheduler scheduler,
    TaskGroup& group,
    int remaining,
    int& completed) {
    co_await scheduler.schedule();

    if (remaining > 1) {
        group.spawn(spawn_recursive_after_schedule(
            scheduler,
            group,
            remaining - 1,
            completed));
    }

    ++completed;
    co_return;
}

Task<int> run_recursive_group(Scheduler scheduler, int count) {
    int completed { 0 };
    TaskGroup group {};
    group.spawn(spawn_recursive_after_schedule(
        scheduler,
        group,
        count,
        completed));
    co_await group.join();
    co_return completed;
}

Task<void> run_cancel_and_join_group(
    Scheduler scheduler,
    bool& cancelled,
    bool& stopRequested) {
    TaskGroup group {};
    const auto token = group.get_stop_token();
    group.spawn(observe_cancellation(scheduler, token, cancelled));
    co_await group.cancel_and_join();
    stopRequested = token.stop_requested();
}

TEST(CmpTaskGroupTest, EmptyGroupJoinsAndTypeIsImmovable) {
    static_assert(!std::copy_constructible<TaskGroup>);
    static_assert(!std::move_constructible<TaskGroup>);

    RunLoop loop {};
    EXPECT_NO_THROW(loop.run(join_empty_group()));
}

TEST(CmpTaskGroupTest, SpawnStartsInlineAndJoinWaitsForEveryTask) {
    RunLoop loop {};
    bool firstStartedInline { false };
    bool secondStartedInline { false };

    const auto events = loop.run(run_scheduled_group(
        loop.get_scheduler(),
        firstStartedInline,
        secondStartedInline));

    EXPECT_TRUE(firstStartedInline);
    EXPECT_TRUE(secondStartedInline);
    EXPECT_EQ(events, (std::vector<int> { 1, 2, 11, 12 }));
}

TEST(CmpTaskGroupTest, JoinTaskRemainsLazyUntilAwaited) {
    RunLoop loop {};
    bool started { false };

    loop.run(verify_lazy_join(started));

    EXPECT_TRUE(started);
}

TEST(CmpTaskGroupTest, SupportsConcurrentAdmission) {
    constexpr int THREAD_COUNT { 4 };
    constexpr int TASKS_PER_THREAD { 250 };
    std::atomic<int> completed { 0 };
    TaskGroup group {};

    {
        std::vector<std::jthread> workers {};
        workers.reserve(THREAD_COUNT);

        for (int thread { 0 }; thread < THREAD_COUNT; ++thread) {
            workers.emplace_back([&group, &completed] {
                for (int task { 0 }; task < TASKS_PER_THREAD; ++task) {
                    group.spawn(increment_atomic(completed));
                }
            });
        }
    }

    RunLoop loop {};
    loop.run(group.join());

    EXPECT_EQ(completed.load(), THREAD_COUNT * TASKS_PER_THREAD);
}

TEST(CmpTaskGroupTest, AllowsRecursiveAdmissionUntilJoinReachesQuiescence) {
    RunLoop loop {};
    bool admitted { false };
    bool candidateStarted { false };

    loop.run(verify_join_allows_recursive_admission(
        loop.get_scheduler(),
        admitted,
        candidateStarted));

    EXPECT_TRUE(admitted);
    EXPECT_TRUE(candidateStarted);

    TaskGroup group {};
    loop.run(group.join());

    int liveCount { 0 };
    EXPECT_THROW(
        group.spawn(hold_token(LifetimeToken { liveCount })),
        std::logic_error);
    EXPECT_EQ(liveCount, 0);
    EXPECT_THROW(loop.run(group.join()), std::logic_error);
}

TEST(CmpTaskGroupTest, RecursiveAdmissionDoesNotGrowTheNativeStack) {
    constexpr int TASK_COUNT { 20'000 };
    RunLoop loop {};

    EXPECT_EQ(
        loop.run(run_recursive_group(
            loop.get_scheduler(),
            TASK_COUNT)),
        TASK_COUNT);
}

TEST(CmpTaskGroupTest, WaitsForAllAndRethrowsFirstExceptionByAdmissionOrder) {
    RunLoop loop {};
    std::vector<std::string> completions {};

    try {
        loop.run(run_failing_group(loop.get_scheduler(), completions));
        FAIL() << "TaskGroup should propagate a child exception";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(error.what(), std::string_view { "first" });
    }

    EXPECT_EQ(
        completions,
        (std::vector<std::string> { "second", "first" }));
    EXPECT_NO_THROW(loop.run(join_empty_group()));
}

TEST(CmpTaskGroupTest, StopTokenReachesExistingAndFutureChildren) {
    RunLoop loop {};
    bool firstCancelled { false };
    bool secondCancelled { false };
    bool firstRequestWon { false };
    bool secondRequestWon { true };

    loop.run(run_cancelled_group(
        loop.get_scheduler(),
        firstCancelled,
        secondCancelled,
        firstRequestWon,
        secondRequestWon));

    EXPECT_TRUE(firstCancelled);
    EXPECT_TRUE(secondCancelled);
    EXPECT_TRUE(firstRequestWon);
    EXPECT_FALSE(secondRequestWon);
}

TEST(CmpTaskGroupTest, CancelAndJoinRequestsStopBeforeWaiting) {
    RunLoop loop {};
    bool cancelled { false };
    bool stopRequested { false };

    loop.run(run_cancel_and_join_group(
        loop.get_scheduler(),
        cancelled,
        stopRequested));

    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(stopRequested);
}

TEST(CmpTaskGroupTest, PublishesCrossThreadCompletion) {
    RunLoop loop {};
    std::atomic<int> total { 0 };
    std::array<std::jthread, 2> workers {};

    loop.run(run_cross_thread_group(workers, total));

    EXPECT_EQ(total.load(), 7);
}

TEST(CmpTaskGroupTest, ImmediateTasksDoNotGrowTheNativeStack) {
    constexpr int TASK_COUNT { 50'000 };
    RunLoop loop {};

    EXPECT_EQ(loop.run(run_immediate_group(TASK_COUNT)), TASK_COUNT);
}

}  // namespace
