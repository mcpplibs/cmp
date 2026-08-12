import std;
import mcpplibs.cmp;

#include <gtest/gtest.h>

namespace {

using mcpplibs::cmp::Task;

template<typename T>
concept SupportsTask = requires {
    typename Task<T>;
};

template<typename T>
concept HasLvalueCoAwait = requires(T& task) {
    task.operator co_await();
};

template<typename T>
concept HasRvalueCoAwait = requires(T&& task) {
    std::move(task).operator co_await();
};

static_assert(SupportsTask<int>);
static_assert(SupportsTask<void>);
static_assert(!SupportsTask<const void>);
static_assert(!SupportsTask<int&>);
static_assert(!SupportsTask<int&&>);
static_assert(!SupportsTask<int[2]>);

static_assert(!std::default_initializable<Task<int>>);
static_assert(!std::copy_constructible<Task<int>>);
static_assert(std::move_constructible<Task<int>>);
static_assert(!std::is_move_assignable_v<Task<int>>);
static_assert(!HasLvalueCoAwait<Task<int>>);
static_assert(HasRvalueCoAwait<Task<int>>);

class TestOperation {
public:
    struct promise_type {
        std::exception_ptr exception_ {};

        [[nodiscard]] TestOperation get_return_object() noexcept;

        [[nodiscard]] constexpr std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        [[nodiscard]] constexpr std::suspend_always final_suspend() const noexcept {
            return {};
        }

        void return_void() const noexcept {}

        void unhandled_exception() noexcept {
            exception_ = std::current_exception();
        }
    };

private:
    using Handle = std::coroutine_handle<promise_type>;

    Handle coroutine_ {};

    explicit TestOperation(Handle coroutine) noexcept
        : coroutine_ { coroutine } {}

public:
    TestOperation(const TestOperation&) = delete;
    TestOperation& operator=(const TestOperation&) = delete;

    TestOperation(TestOperation&& other) noexcept
        : coroutine_ { std::exchange(other.coroutine_, {}) } {}

    TestOperation& operator=(TestOperation&&) = delete;

    ~TestOperation() {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }

    void run();
};

TestOperation TestOperation::promise_type::get_return_object() noexcept {
    return TestOperation { TestOperation::Handle::from_promise(*this) };
}

void TestOperation::run() {
    if (!coroutine_ || coroutine_.done()) {
        throw std::logic_error { "test operation is not runnable" };
    }

    coroutine_.resume();

    if (!coroutine_.done()) {
        throw std::logic_error { "task unexpectedly suspended" };
    }

    if (coroutine_.promise().exception_) {
        std::rethrow_exception(coroutine_.promise().exception_);
    }
}

template<typename T>
TestOperation store_result(Task<T> task, std::optional<T>& result) {
    result.emplace(co_await std::move(task));
}

TestOperation await_task(Task<void> task) {
    co_await std::move(task);
}

Task<int> make_value(bool& started) {
    started = true;
    co_return 42;
}

Task<void> increment(int& value) {
    ++value;
    co_return;
}

Task<int> make_nested_value() {
    bool started { false };
    auto value = co_await make_value(started);
    co_return value + 1;
}

class LifetimeToken {
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

Task<int> throw_error(LifetimeToken token) {
    static_cast<void>(token);
    throw std::runtime_error { "task failed" };
    co_return 0;
}

class MoveOnlyValue {
private:
    int* liveCount_ {};

public:
    int value {};

    MoveOnlyValue(int value, int& liveCount) noexcept
        : liveCount_ { &liveCount }, value { value } {
        ++*liveCount_;
    }

    MoveOnlyValue(const MoveOnlyValue&) = delete;
    MoveOnlyValue& operator=(const MoveOnlyValue&) = delete;

    MoveOnlyValue(MoveOnlyValue&& other) noexcept
        : liveCount_ { std::exchange(other.liveCount_, nullptr) },
          value { other.value } {}

    MoveOnlyValue& operator=(MoveOnlyValue&&) = delete;

    ~MoveOnlyValue() {
        if (liveCount_) {
            --*liveCount_;
        }
    }
};

Task<MoveOnlyValue> make_move_only_value(int& liveCount) {
    co_return MoveOnlyValue { 7, liveCount };
}

Task<void> complete_immediately() {
    co_return;
}

Task<int> complete_many_times(int count) {
    for (int index { 0 }; index < count; ++index) {
        co_await complete_immediately();
    }

    co_return count;
}

TEST(CmpTaskTest, IsLazyAndReturnsValue) {
    bool started { false };
    auto task = make_value(started);
    std::optional<int> result {};

    EXPECT_FALSE(started);

    auto operation = store_result(std::move(task), result);
    EXPECT_FALSE(started);

    operation.run();

    EXPECT_TRUE(started);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(CmpTaskTest, SupportsVoidResults) {
    int value { 0 };
    auto operation = await_task(increment(value));

    EXPECT_EQ(value, 0);
    operation.run();
    EXPECT_EQ(value, 1);
}

TEST(CmpTaskTest, ComposesNestedTasks) {
    std::optional<int> result {};
    auto operation = store_result(make_nested_value(), result);

    operation.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 43);
}

TEST(CmpTaskTest, DestroysAnUnawaitedFrame) {
    int liveCount { 0 };

    {
        auto task = hold_token(LifetimeToken { liveCount });
        EXPECT_EQ(liveCount, 1);
    }

    EXPECT_EQ(liveCount, 0);
}

TEST(CmpTaskTest, MovingTransfersFrameOwnershipOnce) {
    int liveCount { 0 };

    {
        auto first = hold_token(LifetimeToken { liveCount });
        auto second = std::move(first);
        static_cast<void>(second);
        EXPECT_EQ(liveCount, 1);
    }

    EXPECT_EQ(liveCount, 0);
}

TEST(CmpTaskTest, MovesResultBeforeDestroyingFrame) {
    int liveCount { 0 };
    std::optional<MoveOnlyValue> result {};
    auto operation = store_result(make_move_only_value(liveCount), result);

    operation.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value, 7);
    EXPECT_EQ(liveCount, 1);

    result.reset();
    EXPECT_EQ(liveCount, 0);
}

TEST(CmpTaskTest, PropagatesExceptionAndDestroysFrame) {
    int liveCount { 0 };
    std::optional<int> result {};
    auto operation = store_result(
        throw_error(LifetimeToken { liveCount }),
        result);

    EXPECT_EQ(liveCount, 1);
    EXPECT_THROW(operation.run(), std::runtime_error);
    EXPECT_EQ(liveCount, 0);
    EXPECT_FALSE(result.has_value());
}

TEST(CmpTaskTest, SymmetricTransferDoesNotGrowTheStack) {
    constexpr int COMPLETION_COUNT { 1'000'000 };
    std::optional<int> result {};
    auto operation = store_result(
        complete_many_times(COMPLETION_COUNT),
        result);

    operation.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, COMPLETION_COUNT);
}

}  // namespace
