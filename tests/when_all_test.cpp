#include <gtest/gtest.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::when_all;

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

Task<int> mark_started(bool& started, int value) {
    started = true;
    co_return value;
}

Task<void> hold_token(LifetimeToken token) {
    static_cast<void>(token);
    co_return;
}

Task<int> scheduled_value(
    Scheduler scheduler,
    int value,
    std::vector<int>& events) {
    events.push_back(value);
    co_await scheduler.schedule();
    events.push_back(value + 10);
    co_return value;
}

Task<std::unique_ptr<int>> make_move_only_value(int value) {
    co_return std::make_unique<int>(value);
}

Task<std::string> make_text(std::string text) {
    co_return text;
}

Task<void> complete_void(bool& completed) {
    completed = true;
    co_return;
}

Task<int> fail_after_schedules(
    Scheduler scheduler,
    int scheduleCount,
    std::string message,
    std::vector<std::string>& completions) {
    for (int index { 0 }; index < scheduleCount; ++index) {
        co_await scheduler.schedule();
    }

    completions.push_back(message);
    throw std::runtime_error { message };
    co_return 0;
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
        // 启动线程前复制成员，恢复后当前 awaiter 可能立即销毁。
        auto* const worker = worker_;
        *worker = std::jthread { [continuation] {
            continuation.resume();
        } };
    }

    constexpr void await_resume() const noexcept {}
};

Task<int> complete_on_new_thread(std::jthread& worker, int value) {
    co_await ResumeOnNewThread { worker };
    co_return value;
}

Task<int> complete_immediately(int value) {
    co_return value;
}

Task<int> repeat_immediate_join(int count) {
    int sum { 0 };

    for (int index { 0 }; index < count; ++index) {
        auto [first, second] = co_await when_all(
            complete_immediately(1),
            complete_immediately(2));
        sum += first + second;
    }

    co_return sum;
}

Task<int> repeat_immediate_range_join(int count) {
    int sum { 0 };

    for (int index { 0 }; index < count; ++index) {
        std::vector<Task<int>> tasks {};
        tasks.emplace_back(complete_immediately(1));
        tasks.emplace_back(complete_immediately(2));
        auto values = co_await when_all(std::move(tasks));
        sum += values[0] + values[1];
    }

    co_return sum;
}

TEST(CmpWhenAllTest, IsLazyAndReturnsResultsInParameterOrder) {
    RunLoop loop {};
    bool firstStarted { false };
    bool secondStarted { false };
    auto aggregate = when_all(
        mark_started(firstStarted, 1),
        mark_started(secondStarted, 2));

    EXPECT_FALSE(firstStarted);
    EXPECT_FALSE(secondStarted);

    const auto [first, second] = loop.run(std::move(aggregate));

    EXPECT_TRUE(firstStarted);
    EXPECT_TRUE(secondStarted);
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 2);
}

TEST(CmpWhenAllTest, DestroysUnawaitedChildFrames) {
    int liveCount { 0 };

    {
        auto aggregate = when_all(
            hold_token(LifetimeToken { liveCount }),
            hold_token(LifetimeToken { liveCount }));
        static_cast<void>(aggregate);
        EXPECT_EQ(liveCount, 2);
    }

    EXPECT_EQ(liveCount, 0);
}

TEST(CmpWhenAllTest, StartsEveryInputBeforeScheduledCompletion) {
    RunLoop loop {};
    auto scheduler = loop.get_scheduler();
    std::vector<int> events {};

    const auto [first, second] = loop.run(when_all(
        scheduled_value(scheduler, 1, events),
        scheduled_value(scheduler, 2, events)));

    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 2);
    EXPECT_EQ(events, (std::vector<int> { 1, 2, 11, 12 }));
}

TEST(CmpWhenAllTest, SupportsMoveOnlyAndVoidResults) {
    RunLoop loop {};
    bool voidCompleted { false };

    auto [number, text, empty] = loop.run(when_all(
        make_move_only_value(42),
        make_text("cmp"),
        complete_void(voidCompleted)));

    static_assert(std::same_as<decltype(empty), std::monostate>);
    ASSERT_NE(number, nullptr);
    EXPECT_EQ(*number, 42);
    EXPECT_EQ(text, "cmp");
    EXPECT_TRUE(voidCompleted);
}

TEST(CmpWhenAllTest, EmptyInputCompletesWithAnEmptyTuple) {
    RunLoop loop {};
    auto result = loop.run(when_all());

    static_assert(std::same_as<decltype(result), std::tuple<>>);
    EXPECT_EQ(std::tuple_size_v<decltype(result)>, 0U);
}

TEST(CmpWhenAllTest, WaitsForAllAndRethrowsFirstExceptionInParameterOrder) {
    RunLoop loop {};
    auto scheduler = loop.get_scheduler();
    std::vector<std::string> completions {};

    try {
        static_cast<void>(loop.run(when_all(
            fail_after_schedules(scheduler, 2, "first", completions),
            fail_after_schedules(scheduler, 1, "second", completions))));
        FAIL() << "when_all should propagate a child exception";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(error.what(), std::string_view { "first" });
    }

    EXPECT_EQ(
        completions,
        (std::vector<std::string> { "second", "first" }));
    EXPECT_EQ(loop.run(complete_immediately(7)), 7);
}

TEST(CmpWhenAllTest, PublishesResultsFromDifferentThreads) {
    RunLoop loop {};
    std::jthread firstWorker {};
    std::jthread secondWorker {};

    const auto [first, second] = loop.run(when_all(
        complete_on_new_thread(firstWorker, 3),
        complete_on_new_thread(secondWorker, 4)));

    EXPECT_EQ(first, 3);
    EXPECT_EQ(second, 4);
}

TEST(CmpWhenAllTest, ImmediateCompletionDoesNotGrowTheNativeStack) {
    constexpr int JOIN_COUNT { 100'000 };
    RunLoop loop {};

    EXPECT_EQ(loop.run(repeat_immediate_join(JOIN_COUNT)), JOIN_COUNT * 3);
}

TEST(CmpWhenAllTest, RangeOwnsAndDestroysUnawaitedChildFrames) {
    int liveCount { 0 };

    {
        std::vector<Task<void>> tasks {};
        tasks.emplace_back(hold_token(LifetimeToken { liveCount }));
        tasks.emplace_back(hold_token(LifetimeToken { liveCount }));
        auto aggregate = when_all(std::move(tasks));
        static_cast<void>(aggregate);
        EXPECT_EQ(liveCount, 2);
    }

    EXPECT_EQ(liveCount, 0);
}

TEST(CmpWhenAllTest, EmptyRangeCompletesWithAnEmptyVector) {
    RunLoop loop {};
    std::vector<Task<int>> tasks {};
    auto results = loop.run(when_all(std::move(tasks)));

    static_assert(std::same_as<decltype(results), std::vector<int>>);
    EXPECT_TRUE(results.empty());
}

TEST(CmpWhenAllTest, RangeStartsEveryInputAndPreservesIndexOrder) {
    RunLoop loop {};
    auto scheduler = loop.get_scheduler();
    std::vector<int> events {};
    std::vector<Task<int>> tasks {};
    tasks.emplace_back(scheduled_value(scheduler, 1, events));
    tasks.emplace_back(scheduled_value(scheduler, 2, events));
    tasks.emplace_back(scheduled_value(scheduler, 3, events));
    auto aggregate = when_all(std::move(tasks));

    EXPECT_TRUE(events.empty());

    const auto results = loop.run(std::move(aggregate));

    EXPECT_EQ(results, (std::vector<int> { 1, 2, 3 }));
    EXPECT_EQ(events, (std::vector<int> { 1, 2, 3, 11, 12, 13 }));
}

TEST(CmpWhenAllTest, RangeSupportsMoveOnlyAndVoidResults) {
    RunLoop loop {};
    std::vector<Task<std::unique_ptr<int>>> valueTasks {};
    valueTasks.emplace_back(make_move_only_value(4));
    valueTasks.emplace_back(make_move_only_value(5));

    auto values = loop.run(when_all(std::move(valueTasks)));

    ASSERT_EQ(values.size(), 2U);
    ASSERT_NE(values[0], nullptr);
    ASSERT_NE(values[1], nullptr);
    EXPECT_EQ(*values[0], 4);
    EXPECT_EQ(*values[1], 5);

    bool firstCompleted { false };
    bool secondCompleted { false };
    std::vector<Task<void>> voidTasks {};
    voidTasks.emplace_back(complete_void(firstCompleted));
    voidTasks.emplace_back(complete_void(secondCompleted));

    auto emptyValues = loop.run(when_all(std::move(voidTasks)));

    static_assert(std::same_as<
        decltype(emptyValues),
        std::vector<std::monostate>>);
    EXPECT_EQ(emptyValues.size(), 2U);
    EXPECT_TRUE(firstCompleted);
    EXPECT_TRUE(secondCompleted);
}

TEST(CmpWhenAllTest, RangeWaitsForAllAndRethrowsFirstExceptionByIndex) {
    RunLoop loop {};
    auto scheduler = loop.get_scheduler();
    std::vector<std::string> completions {};
    std::vector<Task<int>> tasks {};
    tasks.emplace_back(fail_after_schedules(
        scheduler,
        2,
        "first",
        completions));
    tasks.emplace_back(fail_after_schedules(
        scheduler,
        1,
        "second",
        completions));

    try {
        static_cast<void>(loop.run(when_all(std::move(tasks))));
        FAIL() << "range when_all should propagate a child exception";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(error.what(), std::string_view { "first" });
    }

    EXPECT_EQ(
        completions,
        (std::vector<std::string> { "second", "first" }));
    EXPECT_EQ(loop.run(complete_immediately(8)), 8);
}

TEST(CmpWhenAllTest, RangePublishesResultsFromDifferentThreads) {
    RunLoop loop {};
    std::array<std::jthread, 2> workers {};
    std::vector<Task<int>> tasks {};
    tasks.emplace_back(complete_on_new_thread(workers[0], 6));
    tasks.emplace_back(complete_on_new_thread(workers[1], 7));

    const auto results = loop.run(when_all(std::move(tasks)));

    EXPECT_EQ(results, (std::vector<int> { 6, 7 }));
}

TEST(CmpWhenAllTest, ImmediateRangeCompletionDoesNotGrowTheNativeStack) {
    constexpr int JOIN_COUNT { 50'000 };
    RunLoop loop {};

    EXPECT_EQ(
        loop.run(repeat_immediate_range_join(JOIN_COUNT)),
        JOIN_COUNT * 3);
}

}  // namespace
