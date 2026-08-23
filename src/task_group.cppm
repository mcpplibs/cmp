export module mcpplibs.cmp:task_group;

import std;
import :task;

export namespace mcpplibs::cmp {

class TaskGroup final {
private:
    enum class State {
        unused,
        open,
        joining,
        joined
    };

    class ChildTask final {
    public:
        struct promise_type {
            TaskGroup* group_ {};
            std::exception_ptr exception_ {};

            [[nodiscard]] ChildTask get_return_object() noexcept;

            [[nodiscard]] constexpr std::suspend_always initial_suspend() const noexcept {
                return {};
            }

            class FinalAwaiter final {
            public:
                [[nodiscard]] constexpr bool await_ready() const noexcept {
                    return false;
                }

                [[nodiscard]] std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<promise_type> coroutine) const noexcept;

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

        Handle coroutine_ {};

        explicit ChildTask(Handle coroutine) noexcept
            : coroutine_ { coroutine } {}

    public:
        ChildTask(const ChildTask&) = delete;
        ChildTask& operator=(const ChildTask&) = delete;

        ChildTask(ChildTask&& other) noexcept
            : coroutine_ { std::exchange(other.coroutine_, {}) } {}

        ChildTask& operator=(ChildTask&&) = delete;

        ~ChildTask() {
            if (coroutine_) {
                coroutine_.destroy();
            }
        }

        [[nodiscard]] std::coroutine_handle<> bind(TaskGroup& group) noexcept {
            coroutine_.promise().group_ = &group;
            return coroutine_;
        }

        void rethrow_if_exception() const {
            if (coroutine_.promise().exception_) {
                std::rethrow_exception(coroutine_.promise().exception_);
            }
        }
    };

    class JoinAwaiter final {
    private:
        TaskGroup* group_ {};

    public:
        explicit JoinAwaiter(TaskGroup& group) noexcept
            : group_ { &group } {}

        JoinAwaiter(const JoinAwaiter&) = delete;
        JoinAwaiter& operator=(const JoinAwaiter&) = delete;
        JoinAwaiter(JoinAwaiter&&) = delete;
        JoinAwaiter& operator=(JoinAwaiter&&) = delete;

        [[nodiscard]] bool await_ready() {
            return group_->begin_join_();
        }

        [[nodiscard]] bool await_suspend(
            std::coroutine_handle<> continuation) {
            // 发布后最后一个子任务可能立即恢复并销毁当前 awaiter。
            auto* const group = group_;
            return group->suspend_join_(continuation);
        }

        void await_resume() {
            group_->rethrow_first_exception_();
        }
    };

    std::mutex mutex_ {};
    std::stop_source stopSource_ {};
    std::vector<ChildTask> tasks_ {};
    State state_ { State::unused };
    std::size_t active_ {};
    std::coroutine_handle<> joinContinuation_ {};

    [[nodiscard]] static ChildTask make_child_(Task<void> task);

    [[nodiscard]] bool begin_join_();
    [[nodiscard]] bool suspend_join_(std::coroutine_handle<> continuation);
    [[nodiscard]] std::coroutine_handle<> notify_child_completed_() noexcept;
    void rethrow_first_exception_();

public:
    TaskGroup() = default;
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;
    ~TaskGroup();

    void spawn(Task<void> task);

    [[nodiscard]] Task<void> join();

    [[nodiscard]] std::stop_token get_stop_token() const noexcept {
        return stopSource_.get_token();
    }

    bool request_stop() noexcept {
        return stopSource_.request_stop();
    }
};

inline TaskGroup::ChildTask
TaskGroup::ChildTask::promise_type::get_return_object() noexcept {
    return ChildTask {
        std::coroutine_handle<promise_type>::from_promise(*this)
    };
}

inline std::coroutine_handle<>
TaskGroup::ChildTask::promise_type::FinalAwaiter::await_suspend(
    std::coroutine_handle<promise_type> coroutine) const noexcept {
    // 用对称转移恢复 join，避免在当前帧之外再嵌套一层 resume 调用。
    auto* const group = coroutine.promise().group_;
    return group->notify_child_completed_();
}

inline TaskGroup::ChildTask TaskGroup::make_child_(Task<void> task) {
    co_await std::move(task);
}

inline TaskGroup::~TaskGroup() {
    const std::lock_guard lock { mutex_ };

    if (state_ != State::unused && state_ != State::joined) {
        std::terminate();
    }
}

inline void TaskGroup::spawn(Task<void> task) {
    auto child = make_child_(std::move(task));
    const auto coroutine = child.bind(*this);

    {
        const std::lock_guard lock { mutex_ };

        if (state_ != State::unused && state_ != State::open) {
            throw std::logic_error { "task group is closed" };
        }

        tasks_.push_back(std::move(child));
        ++active_;
        state_ = State::open;
    }

    // 锁外启动，立即完成的子任务会回调当前 group。
    coroutine.resume();
}

inline bool TaskGroup::begin_join_() {
    const std::lock_guard lock { mutex_ };

    if (state_ == State::joining || state_ == State::joined) {
        throw std::logic_error { "task group can only be joined once" };
    }

    if (active_ == 0) {
        state_ = State::joined;
        return true;
    }

    state_ = State::joining;
    return false;
}

inline bool TaskGroup::suspend_join_(
    std::coroutine_handle<> continuation) {
    const std::lock_guard lock { mutex_ };

    if (state_ == State::joined) {
        return false;
    }

    joinContinuation_ = continuation;
    return true;
}

inline std::coroutine_handle<> TaskGroup::notify_child_completed_() noexcept {
    std::coroutine_handle<> continuation {};

    {
        const std::lock_guard lock { mutex_ };

        if (active_ == 0) {
            std::terminate();
        }

        --active_;

        if (active_ == 0 && state_ == State::joining) {
            state_ = State::joined;
            continuation = std::exchange(joinContinuation_, {});
        }
    }

    return continuation ? continuation : std::noop_coroutine();
}

inline void TaskGroup::rethrow_first_exception_() {
    for (const auto& task : tasks_) {
        task.rethrow_if_exception();
    }
}

inline Task<void> TaskGroup::join() {
    co_await JoinAwaiter { *this };
}

}  // namespace mcpplibs::cmp
