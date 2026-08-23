#include <gtest/gtest.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;

using Scheduler = RunLoop::Scheduler;
using Clock = Scheduler::Clock;
using Duration = Scheduler::Duration;
using TimePoint = Scheduler::TimePoint;

using namespace std::chrono_literals;

static_assert(std::default_initializable<RunLoop>);
static_assert(!std::copy_constructible<RunLoop>);
static_assert(!std::move_constructible<RunLoop>);
static_assert(!std::is_copy_assignable_v<RunLoop>);
static_assert(!std::is_move_assignable_v<RunLoop>);

static_assert(!std::default_initializable<Scheduler>);
static_assert(std::copy_constructible<Scheduler>);
static_assert(std::move_constructible<Scheduler>);
static_assert(std::equality_comparable<Scheduler>);
static_assert(std::same_as<Clock, std::chrono::steady_clock>);
static_assert(std::same_as<Duration, Clock::duration>);
static_assert(std::same_as<TimePoint, Clock::time_point>);

Task<int> lazy_value(bool& started) {
    started = true;
    co_return 42;
}

Task<void> increment(int& value) {
    ++value;
    co_return;
}

Task<std::unique_ptr<int>> make_move_only_value() {
    co_return std::make_unique<int>(7);
}

Task<void> do_nothing() {
    co_return;
}

Task<std::thread::id> schedule_once(Scheduler scheduler) {
    co_await scheduler.schedule();
    co_return std::this_thread::get_id();
}

Task<int> schedule_many_times(Scheduler scheduler, int count) {
    for (int index { 0 }; index < count; ++index) {
        co_await scheduler.schedule();
    }

    co_return count;
}

struct ResumeRecord final {
    TimePoint time_ {};
    std::thread::id thread_ {};
};

Task<ResumeRecord> schedule_after_once(
    Scheduler scheduler,
    Duration delay) {
    co_await scheduler.schedule_after(delay);
    co_return ResumeRecord { Clock::now(), std::this_thread::get_id() };
}

Task<ResumeRecord> schedule_at_once(
    Scheduler scheduler,
    TimePoint deadline) {
    co_await scheduler.schedule_at(deadline);
    co_return ResumeRecord { Clock::now(), std::this_thread::get_id() };
}

Task<int> schedule_immediate_deadlines(Scheduler scheduler) {
    int resumed { 0 };

    co_await scheduler.schedule_after(Duration::zero());
    ++resumed;
    co_await scheduler.schedule_after(-1ms);
    ++resumed;
    co_await scheduler.schedule_at(Clock::now());
    ++resumed;
    co_await scheduler.schedule_at(TimePoint::min());
    ++resumed;

    co_return resumed;
}

Task<int> schedule_immediately_many_times(
    Scheduler scheduler,
    int count) {
    for (int index { 0 }; index < count; ++index) {
        co_await scheduler.schedule_after(Duration::zero());
    }

    co_return count;
}

Task<void> fail_after_scheduling(Scheduler scheduler) {
    co_await scheduler.schedule();
    throw std::runtime_error { "scheduled task failed" };
}

Task<void> run_nested(RunLoop& loop) {
    loop.run(do_nothing());
    co_return;
}

class ResumeOnNewThread final {
private:
    std::thread* worker_ {};

public:
    explicit ResumeOnNewThread(std::thread& worker) noexcept
        : worker_ { &worker } {}

    [[nodiscard]] constexpr bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) const {
        // 新线程可能立即恢复协程，因此启动后不再读取 awaiter 成员。
        auto* const worker = worker_;
        *worker = std::thread { [continuation] {
            continuation.resume();
        } };
    }

    constexpr void await_resume() const noexcept {}
};

Task<std::pair<std::thread::id, std::thread::id>> leave_and_return(
    Scheduler scheduler,
    std::thread& worker) {
    co_await ResumeOnNewThread { worker };
    const auto workerThread = std::this_thread::get_id();

    co_await scheduler.schedule();
    co_return std::pair { workerThread, std::this_thread::get_id() };
}

Task<std::thread::id> finish_on_new_thread(std::thread& worker) {
    co_await ResumeOnNewThread { worker };
    co_return std::this_thread::get_id();
}

class ManualSuspend final {
private:
    std::coroutine_handle<>* continuation_ {};
    std::latch* suspended_ {};

public:
    ManualSuspend(
        std::coroutine_handle<>& continuation,
        std::latch& suspended) noexcept
        : continuation_ { &continuation }, suspended_ { &suspended } {}

    [[nodiscard]] constexpr bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) const noexcept {
        *continuation_ = continuation;
        suspended_->count_down();
    }

    constexpr void await_resume() const noexcept {}
};

Task<void> suspend_manually(
    std::coroutine_handle<>& continuation,
    std::latch& suspended) {
    co_await ManualSuspend { continuation, suspended };
}

class EagerOperation final {
public:
    struct promise_type {
        [[nodiscard]] EagerOperation get_return_object() noexcept;

        [[nodiscard]] constexpr std::suspend_never initial_suspend() const noexcept {
            return {};
        }

        [[nodiscard]] constexpr std::suspend_always final_suspend() const noexcept {
            return {};
        }

        constexpr void return_void() const noexcept {}

        [[noreturn]] void unhandled_exception() const noexcept {
            std::terminate();
        }
    };

private:
    using Handle = std::coroutine_handle<promise_type>;

    Handle coroutine_ {};

    explicit EagerOperation(Handle coroutine) noexcept
        : coroutine_ { coroutine } {}

public:
    EagerOperation(const EagerOperation&) = delete;
    EagerOperation& operator=(const EagerOperation&) = delete;

    EagerOperation(EagerOperation&& other) noexcept
        : coroutine_ { std::exchange(other.coroutine_, {}) } {}

    EagerOperation& operator=(EagerOperation&&) = delete;

    ~EagerOperation() {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }
};

EagerOperation EagerOperation::promise_type::get_return_object() noexcept {
    return EagerOperation {
        EagerOperation::Handle::from_promise(*this)
    };
}

EagerOperation mark_after(
    Scheduler scheduler,
    Duration delay,
    bool& resumed) {
    co_await scheduler.schedule_after(delay);
    resumed = true;
}

EagerOperation record_at(
    Scheduler scheduler,
    TimePoint deadline,
    int value,
    std::vector<int>& order) {
    co_await scheduler.schedule_at(deadline);
    order.push_back(value);
}

EagerOperation signal_after(
    Scheduler scheduler,
    Duration delay,
    std::thread::id& resumedThread,
    std::binary_semaphore& resumed) {
    co_await scheduler.schedule_after(delay);
    resumedThread = std::this_thread::get_id();
    resumed.release();
}

Task<void> run_timer_group(
    Scheduler scheduler,
    std::vector<EagerOperation>& operations,
    std::vector<int>& order) {
    const auto firstDeadline = Clock::now() + 100ms;
    const auto equalDeadline = firstDeadline + 20ms;

    operations.emplace_back(record_at(
        scheduler,
        equalDeadline,
        3,
        order));
    operations.emplace_back(record_at(
        scheduler,
        firstDeadline,
        1,
        order));
    operations.emplace_back(record_at(
        scheduler,
        equalDeadline,
        2,
        order));

    co_await scheduler.schedule_at(equalDeadline + 20ms);
}

Task<void> leave_pending_max_timer(
    Scheduler scheduler,
    std::optional<EagerOperation>& operation,
    bool& resumed) {
    operation.emplace(mark_after(
        scheduler,
        Duration::max(),
        resumed));
    co_await scheduler.schedule();
}

EagerOperation enqueue_external_work(Scheduler scheduler) {
    co_await scheduler.schedule();
}

Task<void> leave_outstanding_work(
    Scheduler scheduler,
    std::optional<EagerOperation>& operation) {
    operation.emplace(enqueue_external_work(std::move(scheduler)));
    co_return;
}

Scheduler make_expired_scheduler() {
    RunLoop loop {};
    return loop.get_scheduler();
}

TEST(CmpRunLoopTest, RunsLazyValueAndVoidTasks) {
    RunLoop loop {};
    bool started { false };
    int value { 0 };
    auto task = lazy_value(started);

    EXPECT_FALSE(started);
    EXPECT_EQ(loop.run(std::move(task)), 42);
    EXPECT_TRUE(started);

    loop.run(increment(value));
    EXPECT_EQ(value, 1);
}

TEST(CmpRunLoopTest, ReturnsMoveOnlyValues) {
    RunLoop loop {};
    auto value = loop.run(make_move_only_value());

    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 7);
}

TEST(CmpRunLoopTest, SchedulerIdentityMatchesItsRunLoop) {
    RunLoop first {};
    RunLoop second {};

    EXPECT_EQ(first.get_scheduler(), first.get_scheduler());
    EXPECT_NE(first.get_scheduler(), second.get_scheduler());
}

TEST(CmpRunLoopTest, SchedulingResumesOnTheCallingThread) {
    RunLoop loop {};
    const auto callingThread = std::this_thread::get_id();
    auto awaiter = loop.get_scheduler().schedule();

    EXPECT_FALSE(awaiter.await_ready());
    EXPECT_EQ(loop.run(schedule_once(loop.get_scheduler())), callingThread);
}

TEST(CmpRunLoopTest, SchedulingReturnsFromAnotherThread) {
    RunLoop loop {};
    std::thread worker {};
    const auto callingThread = std::this_thread::get_id();

    const auto [workerThread, resumedThread] = loop.run(
        leave_and_return(loop.get_scheduler(), worker));

    if (worker.joinable()) {
        worker.join();
    }

    EXPECT_NE(workerThread, callingThread);
    EXPECT_EQ(resumedThread, callingThread);
}

TEST(CmpRunLoopTest, TimedSchedulingNeverResumesBeforeItsDeadline) {
    constexpr auto DELAY = 15ms;
    RunLoop loop {};
    const auto callingThread = std::this_thread::get_id();

    const auto relativeStart = Clock::now();
    const auto relative = loop.run(schedule_after_once(
        loop.get_scheduler(),
        DELAY));

    EXPECT_FALSE(relative.time_ < relativeStart + DELAY);
    EXPECT_EQ(relative.thread_, callingThread);

    const auto absoluteDeadline = Clock::now() + DELAY;
    const auto absolute = loop.run(schedule_at_once(
        loop.get_scheduler(),
        absoluteDeadline));

    EXPECT_FALSE(absolute.time_ < absoluteDeadline);
    EXPECT_EQ(absolute.thread_, callingThread);
}

TEST(CmpRunLoopTest, ImmediateTimersStillSuspendAndQueue) {
    RunLoop loop {};
    const auto scheduler = loop.get_scheduler();

    EXPECT_FALSE(
        scheduler.schedule_after(Duration::zero()).await_ready());
    EXPECT_FALSE(
        scheduler.schedule_at(TimePoint::min()).await_ready());
    EXPECT_EQ(loop.run(schedule_immediate_deadlines(scheduler)), 4);
}

TEST(CmpRunLoopTest, DispatchesMultipleAndEqualDeadlineTimersOnce) {
    RunLoop loop {};
    std::vector<EagerOperation> operations {};
    std::vector<int> order {};
    operations.reserve(3);
    order.reserve(3);

    loop.run(run_timer_group(
        loop.get_scheduler(),
        operations,
        order));

    ASSERT_EQ(order.size(), 3U);
    EXPECT_EQ(order.front(), 1);
    std::ranges::sort(order);
    EXPECT_EQ(order, (std::vector<int> { 1, 2, 3 }));
}

TEST(CmpRunLoopTest, NewEarlierTimerWakesTheWaitingRunLoop) {
    RunLoop loop {};
    std::coroutine_handle<> rootContinuation {};
    std::latch rootSuspended { 1 };
    std::exception_ptr driverException {};

    std::thread driver { [&] {
        try {
            loop.run(suspend_manually(
                rootContinuation,
                rootSuspended));
        } catch (...) {
            driverException = std::current_exception();
        }
    } };
    const auto driverThread = driver.get_id();

    rootSuspended.wait();
    ASSERT_TRUE(rootContinuation);

    bool longTimerResumed { false };
    std::optional<EagerOperation> longTimer {};
    longTimer.emplace(mark_after(
        loop.get_scheduler(),
        30s,
        longTimerResumed));

    // 让驱动线程先进入长定时等待，再从本线程注册更早期限。
    std::this_thread::sleep_for(20ms);

    std::thread::id earlyTimerThread {};
    std::binary_semaphore earlyTimerResumed { 0 };
    std::optional<EagerOperation> earlyTimer {};
    earlyTimer.emplace(signal_after(
        loop.get_scheduler(),
        10ms,
        earlyTimerThread,
        earlyTimerResumed));

    const bool wokeForEarlierTimer = earlyTimerResumed.try_acquire_for(1s);

    // 根任务从外部完成，使失败路径也能确定退出并清理两个 Timer。
    rootContinuation.resume();
    driver.join();

    EXPECT_TRUE(wokeForEarlierTimer);
    if (wokeForEarlierTimer) {
        EXPECT_EQ(earlyTimerThread, driverThread);
    }
    EXPECT_FALSE(longTimerResumed);
    ASSERT_TRUE(driverException);
    EXPECT_THROW(
        std::rethrow_exception(driverException),
        std::logic_error);

    earlyTimer.reset();
    longTimer.reset();
    loop.run(do_nothing());
}

TEST(CmpRunLoopTest, RejectsInvalidSchedulersForTimedScheduling) {
    RunLoop owner {};
    RunLoop driver {};
    const auto expired = make_expired_scheduler();

    EXPECT_THROW(
        driver.run(schedule_after_once(
            owner.get_scheduler(),
            Duration::zero())),
        std::logic_error);
    EXPECT_THROW(
        driver.run(schedule_at_once(
            owner.get_scheduler(),
            TimePoint::min())),
        std::logic_error);
    EXPECT_THROW(
        driver.run(schedule_after_once(
            expired,
            Duration::zero())),
        std::logic_error);
    EXPECT_THROW(
        driver.run(schedule_at_once(
            expired,
            TimePoint::min())),
        std::logic_error);

    driver.run(do_nothing());
}

TEST(CmpRunLoopTest, PendingMaximumTimerIsCleanedWithoutWraparound) {
    RunLoop loop {};
    std::optional<EagerOperation> operation {};
    bool resumed { false };

    EXPECT_THROW(
        loop.run(leave_pending_max_timer(
            loop.get_scheduler(),
            operation,
            resumed)),
        std::logic_error);

    EXPECT_FALSE(resumed);
    operation.reset();
    loop.run(do_nothing());
}

TEST(CmpRunLoopTest, RootCompletionWakesTheCallingThread) {
    RunLoop loop {};
    std::thread worker {};
    const auto callingThread = std::this_thread::get_id();

    const auto completionThread = loop.run(finish_on_new_thread(worker));

    if (worker.joinable()) {
        worker.join();
    }

    EXPECT_NE(completionThread, callingThread);
}

TEST(CmpRunLoopTest, PropagatesExceptionAfterScheduling) {
    RunLoop loop {};
    bool started { false };

    EXPECT_THROW(
        loop.run(fail_after_scheduling(loop.get_scheduler())),
        std::runtime_error);

    EXPECT_EQ(loop.run(lazy_value(started)), 42);
    EXPECT_TRUE(started);
}

TEST(CmpRunLoopTest, ReusesTheLoopSequentially) {
    RunLoop loop {};
    bool firstStarted { false };
    bool secondStarted { false };

    EXPECT_EQ(loop.run(lazy_value(firstStarted)), 42);
    EXPECT_EQ(loop.run(lazy_value(secondStarted)), 42);
    EXPECT_TRUE(firstStarted);
    EXPECT_TRUE(secondStarted);
}

TEST(CmpRunLoopTest, RejectsNestedRunAndRemainsReusable) {
    RunLoop loop {};
    bool started { false };

    EXPECT_THROW(loop.run(run_nested(loop)), std::logic_error);
    EXPECT_EQ(loop.run(lazy_value(started)), 42);
    EXPECT_TRUE(started);
}

TEST(CmpRunLoopTest, RejectsConcurrentRunAndRemainsReusable) {
    RunLoop loop {};
    std::coroutine_handle<> continuation {};
    std::latch suspended { 1 };
    std::exception_ptr driverException {};

    std::thread driver { [&] {
        try {
            loop.run(suspend_manually(continuation, suspended));
        } catch (...) {
            driverException = std::current_exception();
        }
    } };

    suspended.wait();
    ASSERT_TRUE(continuation);
    EXPECT_THROW(loop.run(do_nothing()), std::logic_error);

    continuation.resume();
    driver.join();

    EXPECT_FALSE(driverException);
    loop.run(do_nothing());
}

TEST(CmpRunLoopTest, RejectsSchedulerWithoutItsActiveRun) {
    RunLoop owner {};
    RunLoop driver {};

    EXPECT_THROW(
        driver.run(schedule_once(owner.get_scheduler())),
        std::logic_error);
}

TEST(CmpRunLoopTest, RejectsSchedulerAfterRunLoopDestruction) {
    RunLoop driver {};

    EXPECT_THROW(
        driver.run(schedule_once(make_expired_scheduler())),
        std::logic_error);
}

TEST(CmpRunLoopTest, RejectsOutstandingWorkAndRemainsReusable) {
    RunLoop loop {};
    std::optional<EagerOperation> operation {};

    EXPECT_THROW(
        loop.run(leave_outstanding_work(
            loop.get_scheduler(),
            operation)),
        std::logic_error);

    operation.reset();
    loop.run(do_nothing());
}

TEST(CmpRunLoopTest, RepeatedSchedulingDoesNotGrowTheStack) {
    constexpr int SCHEDULE_COUNT { 100'000 };
    RunLoop loop {};

    EXPECT_EQ(
        loop.run(schedule_many_times(
            loop.get_scheduler(),
            SCHEDULE_COUNT)),
        SCHEDULE_COUNT);
}

TEST(CmpRunLoopTest, RepeatedImmediateTimersDoNotGrowTheStack) {
    constexpr int TIMER_COUNT { 100'000 };
    RunLoop loop {};

    EXPECT_EQ(
        loop.run(schedule_immediately_many_times(
            loop.get_scheduler(),
            TIMER_COUNT)),
        TIMER_COUNT);
}

}  // namespace
