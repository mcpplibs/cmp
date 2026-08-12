import std;
import mcpplibs.cmp;

#include <gtest/gtest.h>

namespace {

using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;

using Scheduler = RunLoop::Scheduler;

static_assert(std::default_initializable<RunLoop>);
static_assert(!std::copy_constructible<RunLoop>);
static_assert(!std::move_constructible<RunLoop>);
static_assert(!std::is_copy_assignable_v<RunLoop>);
static_assert(!std::is_move_assignable_v<RunLoop>);

static_assert(!std::default_initializable<Scheduler>);
static_assert(std::copy_constructible<Scheduler>);
static_assert(std::move_constructible<Scheduler>);
static_assert(std::equality_comparable<Scheduler>);

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
    std::jthread* worker_ {};

public:
    explicit ResumeOnNewThread(std::jthread& worker) noexcept
        : worker_ { &worker } {}

    [[nodiscard]] constexpr bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) const {
        // 新线程可能立即恢复协程，因此启动后不再读取 awaiter 成员。
        auto* const worker = worker_;
        *worker = std::jthread { [continuation] {
            continuation.resume();
        } };
    }

    constexpr void await_resume() const noexcept {}
};

Task<std::pair<std::thread::id, std::thread::id>> leave_and_return(
    Scheduler scheduler,
    std::jthread& worker) {
    co_await ResumeOnNewThread { worker };
    const auto workerThread = std::this_thread::get_id();

    co_await scheduler.schedule();
    co_return std::pair { workerThread, std::this_thread::get_id() };
}

Task<std::thread::id> finish_on_new_thread(std::jthread& worker) {
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
    std::jthread worker {};
    const auto callingThread = std::this_thread::get_id();

    const auto [workerThread, resumedThread] = loop.run(
        leave_and_return(loop.get_scheduler(), worker));

    if (worker.joinable()) {
        worker.join();
    }

    EXPECT_NE(workerThread, callingThread);
    EXPECT_EQ(resumedThread, callingThread);
}

TEST(CmpRunLoopTest, RootCompletionWakesTheCallingThread) {
    RunLoop loop {};
    std::jthread worker {};
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

    std::jthread driver { [&] {
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

}  // namespace
