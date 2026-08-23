export module mcpplibs.cmp:when_all;

import std;
import :task;

namespace mcpplibs::cmp::detail {

template<typename T>
using WhenAllValue = std::conditional_t<
    std::same_as<T, void>,
    std::monostate,
    T>;

class JoinCounter final {
private:
    // 多出的 1 是注册哨兵，防止同步子任务过早恢复父协程。
    std::atomic<std::size_t> remaining_;
    std::coroutine_handle<> continuation_ {};

public:
    explicit JoinCounter(std::size_t count) noexcept
        : remaining_ { count + 1 } {}

    [[nodiscard]] bool try_await(
        std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
        return remaining_.fetch_sub(1, std::memory_order_acq_rel) > 1;
    }

    void notify_child_completed() noexcept {
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            continuation_.resume();
        }
    }
};

template<typename T>
class JoinResultStorage {
private:
    std::optional<T> result_ {};

public:
    template<typename U>
    requires std::constructible_from<T, U&&>
    void return_value(U&& value)
        noexcept(std::is_nothrow_constructible_v<T, U&&>) {
        result_.emplace(std::forward<U>(value));
    }

    T take_result() {
        if (!result_) {
            throw std::logic_error {
                "when_all child completed without a result"
            };
        }

        return std::move(*result_);
    }
};

template<>
class JoinResultStorage<void> {
public:
    constexpr void return_void() const noexcept {}

    [[nodiscard]] constexpr std::monostate take_result() const noexcept {
        return {};
    }
};

template<typename T>
class JoinTask final {
public:
    struct promise_type : JoinResultStorage<T> {
        JoinCounter* counter_ {};
        std::exception_ptr exception_ {};

        [[nodiscard]] JoinTask get_return_object() noexcept;

        [[nodiscard]] constexpr std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        class FinalAwaiter final {
        public:
            [[nodiscard]] constexpr bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(
                std::coroutine_handle<promise_type> coroutine) const noexcept {
                coroutine.promise().counter_->notify_child_completed();
            }

            constexpr void await_resume() const noexcept {}
        };

        [[nodiscard]] constexpr FinalAwaiter final_suspend() const noexcept {
            return {};
        }

        void unhandled_exception() noexcept {
            exception_ = std::current_exception();
        }
    };

private:
    using Handle = std::coroutine_handle<promise_type>;

    Handle coroutine_ {};

    explicit JoinTask(Handle coroutine) noexcept
        : coroutine_ { coroutine } {}

public:
    JoinTask(const JoinTask&) = delete;
    JoinTask& operator=(const JoinTask&) = delete;

    JoinTask(JoinTask&& other) noexcept
        : coroutine_ { std::exchange(other.coroutine_, {}) } {}

    JoinTask& operator=(JoinTask&&) = delete;

    ~JoinTask() {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }

    void start(JoinCounter& counter) noexcept {
        coroutine_.promise().counter_ = &counter;
        coroutine_.resume();
    }

    void rethrow_if_exception() const {
        if (coroutine_.promise().exception_) {
            std::rethrow_exception(coroutine_.promise().exception_);
        }
    }

    WhenAllValue<T> take_result() {
        return coroutine_.promise().take_result();
    }
};

template<typename T>
JoinTask<T> JoinTask<T>::promise_type::get_return_object() noexcept {
    return JoinTask {
        std::coroutine_handle<promise_type>::from_promise(*this)
    };
}

template<typename T>
JoinTask<T> make_join_task(Task<T> task) {
    if constexpr (std::same_as<T, void>) {
        co_await std::move(task);
        co_return;
    } else {
        co_return co_await std::move(task);
    }
}

template<typename... Results>
class WhenAllAwaiter final {
private:
    JoinCounter counter_ { sizeof...(Results) };
    std::tuple<JoinTask<Results>...> tasks_;

public:
    explicit WhenAllAwaiter(Task<Results>... tasks)
        : tasks_ { make_join_task(std::move(tasks))... } {}

    WhenAllAwaiter(const WhenAllAwaiter&) = delete;
    WhenAllAwaiter& operator=(const WhenAllAwaiter&) = delete;
    WhenAllAwaiter(WhenAllAwaiter&&) = delete;
    WhenAllAwaiter& operator=(WhenAllAwaiter&&) = delete;

    [[nodiscard]] static constexpr bool await_ready() noexcept {
        return sizeof...(Results) == 0;
    }

    [[nodiscard]] bool await_suspend(
        std::coroutine_handle<> continuation) noexcept {
        std::apply(
            [this](auto&... tasks) noexcept {
                (tasks.start(counter_), ...);
            },
            tasks_);

        // 此调用后最后一个子任务可能立即恢复并销毁当前 awaiter。
        return counter_.try_await(continuation);
    }

    std::tuple<WhenAllValue<Results>...> await_resume() {
        // 先按参数顺序检查异常，避免先移动部分结果再发现子任务失败。
        std::apply(
            [](const auto&... tasks) {
                (tasks.rethrow_if_exception(), ...);
            },
            tasks_);

        return std::apply(
            [](auto&... tasks) {
                return std::tuple<WhenAllValue<Results>...> {
                    tasks.take_result()...
                };
            },
            tasks_);
    }
};

}  // namespace mcpplibs::cmp::detail

export namespace mcpplibs::cmp {

template<typename... Results>
[[nodiscard]] Task<std::tuple<detail::WhenAllValue<Results>...>> when_all(
    Task<Results>... tasks) {
    co_return co_await detail::WhenAllAwaiter<Results...> {
        std::move(tasks)...
    };
}

}  // namespace mcpplibs::cmp
