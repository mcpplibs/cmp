export module mcpplibs.cmp:task;

import std;

export namespace mcpplibs::cmp {

// 完成时对称转移到等待方，避免嵌套 Task 持续增长原生调用栈
template<typename T = void>
requires (
    std::same_as<T, void> ||
    (std::is_object_v<T> && !std::is_array_v<T>)
)
class [[nodiscard]] Task {
public:
    struct promise_type {
        std::optional<T> result_ {};
        std::exception_ptr exception_ {};
        std::coroutine_handle<> continuation_ { std::noop_coroutine() };

        [[nodiscard]] Task get_return_object() noexcept;

        [[nodiscard]] constexpr std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        class FinalAwaiter {
        public:
            [[nodiscard]] constexpr bool await_ready() const noexcept {
                return false;
            }

            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> coroutine) const noexcept {
                return coroutine.promise().continuation_;
            }

            constexpr void await_resume() const noexcept {}
        };

        [[nodiscard]] constexpr FinalAwaiter final_suspend() const noexcept {
            return {};
        }

        template<typename U>
        requires std::constructible_from<T, U&&>
        void return_value(U&& value)
            noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            result_.emplace(std::forward<U>(value));
        }

        void unhandled_exception() noexcept {
            exception_ = std::current_exception();
        }
    };

private:
    using Handle = std::coroutine_handle<promise_type>;

    class Awaiter {
    private:
        Handle coroutine_ {};

    public:
        explicit Awaiter(Handle coroutine) noexcept
            : coroutine_ { coroutine } {}

        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;

        Awaiter(Awaiter&& other) noexcept
            : coroutine_ { std::exchange(other.coroutine_, {}) } {}

        Awaiter& operator=(Awaiter&&) = delete;

        ~Awaiter() {
            if (coroutine_) {
                coroutine_.destroy();
            }
        }

        [[nodiscard]] constexpr bool await_ready() const noexcept {
            return false;
        }

        [[nodiscard]] std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> continuation) noexcept {
            coroutine_.promise().continuation_ = continuation;
            return coroutine_;
        }

        T await_resume() {
            auto& promise = coroutine_.promise();

            if (promise.exception_) {
                std::rethrow_exception(promise.exception_);
            }

            return std::move(*promise.result_);
        }
    };

    Handle coroutine_ {};

    explicit Task(Handle coroutine) noexcept
        : coroutine_ { coroutine } {}

public:
    Task() = delete;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept
        : coroutine_ { std::exchange(other.coroutine_, {}) } {}

    Task& operator=(Task&&) = delete;

    ~Task() {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }

    [[nodiscard]] auto operator co_await() && noexcept {
        if (!coroutine_) {
            std::terminate();
        }

        return Awaiter { std::exchange(coroutine_, {}) };
    }
};

template<typename T>
requires (
    std::same_as<T, void> ||
    (std::is_object_v<T> && !std::is_array_v<T>)
)
Task<T> Task<T>::promise_type::get_return_object() noexcept {
    return Task {
        std::coroutine_handle<promise_type>::from_promise(*this)
    };
}

template<>
class [[nodiscard]] Task<void> {
public:
    struct promise_type {
        std::exception_ptr exception_ {};
        std::coroutine_handle<> continuation_ { std::noop_coroutine() };

        [[nodiscard]] Task get_return_object() noexcept;

        [[nodiscard]] constexpr std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        class FinalAwaiter {
        public:
            [[nodiscard]] constexpr bool await_ready() const noexcept {
                return false;
            }

            [[nodiscard]] std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> coroutine) const noexcept {
                return coroutine.promise().continuation_;
            }

            constexpr void await_resume() const noexcept {}
        };

        [[nodiscard]] constexpr FinalAwaiter final_suspend() const noexcept {
            return {};
        }

        constexpr void return_void() const noexcept {}

        void unhandled_exception() noexcept {
            exception_ = std::current_exception();
        }
    };

private:
    using Handle = std::coroutine_handle<promise_type>;

    class Awaiter {
    private:
        Handle coroutine_ {};

    public:
        explicit Awaiter(Handle coroutine) noexcept
            : coroutine_ { coroutine } {}

        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;

        Awaiter(Awaiter&& other) noexcept
            : coroutine_ { std::exchange(other.coroutine_, {}) } {}

        Awaiter& operator=(Awaiter&&) = delete;

        ~Awaiter() {
            if (coroutine_) {
                coroutine_.destroy();
            }
        }

        [[nodiscard]] constexpr bool await_ready() const noexcept {
            return false;
        }

        [[nodiscard]] std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> continuation) noexcept {
            coroutine_.promise().continuation_ = continuation;
            return coroutine_;
        }

        void await_resume() {
            auto& promise = coroutine_.promise();

            if (promise.exception_) {
                std::rethrow_exception(promise.exception_);
            }
        }
    };

    Handle coroutine_ {};

    explicit Task(Handle coroutine) noexcept
        : coroutine_ { coroutine } {}

public:
    Task() = delete;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept
        : coroutine_ { std::exchange(other.coroutine_, {}) } {}

    Task& operator=(Task&&) = delete;

    ~Task() {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }

    [[nodiscard]] auto operator co_await() && noexcept {
        if (!coroutine_) {
            std::terminate();
        }

        return Awaiter { std::exchange(coroutine_, {}) };
    }
};

inline Task<void> Task<void>::promise_type::get_return_object() noexcept {
    return Task {
        std::coroutine_handle<promise_type>::from_promise(*this)
    };
}

}  // namespace mcpplibs::cmp
