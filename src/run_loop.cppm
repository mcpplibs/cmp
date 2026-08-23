export module mcpplibs.cmp:run_loop;

import std;
import :task;

namespace mcpplibs::cmp::detail {

using RunLoopClock = std::chrono::steady_clock;

struct TimerEntry final {
    RunLoopClock::time_point deadline_ {};
    std::coroutine_handle<> continuation_ {};
};

struct TimerEntryLater final {
    [[nodiscard]] bool operator()(
        const TimerEntry& left,
        const TimerEntry& right) const noexcept {
        return left.deadline_ > right.deadline_;
    }
};

struct RootCompletionBase {
    bool completed_ { false };
    std::exception_ptr exception_ {};
};

template<typename T>
struct RootCompletion final : RootCompletionBase {
    std::optional<T> result_ {};
};

template<>
struct RootCompletion<void> final : RootCompletionBase {};

class RunLoopState final {
private:
    std::mutex mutex_ {};
    std::condition_variable condition_ {};
    std::deque<std::coroutine_handle<>> ready_ {};
    // ponytail: v1 堆不支持任意删除；加入取消时改为惰性失效或可删除结构。
    std::priority_queue<
        TimerEntry,
        std::vector<TimerEntry>,
        TimerEntryLater> timers_ {};
    bool running_ { false };

public:
    void begin_run() {
        const std::lock_guard lock { mutex_ };

        if (running_) {
            throw std::logic_error { "run loop is already running" };
        }

        if (!ready_.empty() || !timers_.empty()) {
            throw std::logic_error { "run loop contains abandoned work" };
        }

        running_ = true;
    }

    void enqueue(std::coroutine_handle<> coroutine) {
        if (!coroutine) {
            throw std::invalid_argument { "cannot schedule an empty coroutine" };
        }

        {
            const std::lock_guard lock { mutex_ };

            if (!running_) {
                throw std::logic_error { "scheduler has no active run" };
            }

            ready_.push_back(coroutine);
        }

        condition_.notify_one();
    }

    void enqueue_after(
        RunLoopClock::duration delay,
        std::coroutine_handle<> coroutine) {
        const auto now = RunLoopClock::now();

        if (delay <= RunLoopClock::duration::zero()) {
            enqueue_at(now, coroutine);
            return;
        }

        const auto deadline = now > RunLoopClock::time_point::max() - delay
            ? RunLoopClock::time_point::max()
            : now + delay;
        enqueue_at(deadline, coroutine);
    }

    void enqueue_at(
        RunLoopClock::time_point deadline,
        std::coroutine_handle<> coroutine) {
        if (!coroutine) {
            throw std::invalid_argument { "cannot schedule an empty coroutine" };
        }

        bool shouldNotify { false };

        {
            const std::lock_guard lock { mutex_ };

            if (!running_) {
                throw std::logic_error { "scheduler has no active run" };
            }

            if (deadline <= RunLoopClock::now()) {
                ready_.push_back(coroutine);
                shouldNotify = true;
            } else {
                shouldNotify = timers_.empty() ||
                    deadline < timers_.top().deadline_;
                timers_.push(TimerEntry { deadline, coroutine });
            }
        }

        if (shouldNotify) {
            condition_.notify_one();
        }
    }

    void complete(RootCompletionBase& completion) noexcept {
        // 通过互斥量发布完成状态，以及此前写入的结果或异常
        {
            const std::lock_guard lock { mutex_ };
            completion.completed_ = true;
        }

        condition_.notify_one();
    }

    void drive(RootCompletionBase& completion) {
        while (true) {
            std::coroutine_handle<> coroutine {};

            {
                std::unique_lock lock { mutex_ };

                while (true) {
                    if (completion.completed_) {
                        if (!ready_.empty() || !timers_.empty()) {
                            throw std::logic_error {
                                "root task completed with outstanding work"
                            };
                        }

                        running_ = false;
                        return;
                    }

                    if (!timers_.empty() &&
                        timers_.top().deadline_ <= RunLoopClock::now()) {
                        // 先入 FIFO 再出队，避免 ready 或 Timer 任一侧长期饥饿。
                        ready_.push_back(timers_.top().continuation_);
                        timers_.pop();
                    }

                    if (!ready_.empty()) {
                        coroutine = ready_.front();
                        ready_.pop_front();
                        break;
                    }

                    if (timers_.empty()) {
                        condition_.wait(lock);
                    } else {
                        // 等待会解锁，必须复制期限，不能持有可能因堆重排失效的引用。
                        const auto deadline = timers_.top().deadline_;
                        condition_.wait_until(lock, deadline);
                    }
                }
            }

            if (!coroutine || coroutine.done()) {
                throw std::logic_error { "run loop contains a non-runnable coroutine" };
            }

            // 锁外恢复，协程再次入队时不会与 RunLoop 自锁
            coroutine.resume();
        }
    }

    void abort_run() noexcept {
        const std::lock_guard lock { mutex_ };
        ready_.clear();
        timers_ = {};
        running_ = false;
    }
};

class RunGuard final {
private:
    std::shared_ptr<RunLoopState> state_ {};
    bool active_ { true };

public:
    explicit RunGuard(std::shared_ptr<RunLoopState> state)
        : state_ { std::move(state) } {
        state_->begin_run();
    }

    RunGuard(const RunGuard&) = delete;
    RunGuard& operator=(const RunGuard&) = delete;
    RunGuard(RunGuard&&) = delete;
    RunGuard& operator=(RunGuard&&) = delete;

    ~RunGuard() {
        if (active_) {
            state_->abort_run();
        }
    }

    void release() noexcept {
        active_ = false;
    }
};

class RootOperation final {
public:
    struct promise_type {
        RunLoopState* state_ {};
        RootCompletionBase* completion_ {};

        [[nodiscard]] RootOperation get_return_object() noexcept;

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
                auto* const state = coroutine.promise().state_;
                auto* const completion = coroutine.promise().completion_;
                state->complete(*completion);
            }

            constexpr void await_resume() const noexcept {}
        };

        [[nodiscard]] constexpr FinalAwaiter final_suspend() const noexcept {
            return {};
        }

        constexpr void return_void() const noexcept {}

        void unhandled_exception() noexcept {
            completion_->exception_ = std::current_exception();
        }
    };

private:
    using Handle = std::coroutine_handle<promise_type>;

    Handle coroutine_ {};

    explicit RootOperation(Handle coroutine) noexcept
        : coroutine_ { coroutine } {}

public:
    RootOperation(const RootOperation&) = delete;
    RootOperation& operator=(const RootOperation&) = delete;

    RootOperation(RootOperation&& other) noexcept
        : coroutine_ { std::exchange(other.coroutine_, {}) } {}

    RootOperation& operator=(RootOperation&&) = delete;

    ~RootOperation() {
        if (coroutine_) {
            coroutine_.destroy();
        }
    }

    void bind(RunLoopState& state, RootCompletionBase& completion) noexcept {
        coroutine_.promise().state_ = &state;
        coroutine_.promise().completion_ = &completion;
    }

    [[nodiscard]] std::coroutine_handle<> handle() const noexcept {
        return coroutine_;
    }
};

inline RootOperation RootOperation::promise_type::get_return_object() noexcept {
    return RootOperation {
        std::coroutine_handle<promise_type>::from_promise(*this)
    };
}

template<typename T>
RootOperation make_root_operation(Task<T> task, RootCompletion<T>& completion) {
    if constexpr (std::same_as<T, void>) {
        co_await std::move(task);
    } else {
        completion.result_.emplace(co_await std::move(task));
    }
}

}  // namespace mcpplibs::cmp::detail

export namespace mcpplibs::cmp {

class RunLoop final {
public:
    class Scheduler final {
    public:
        using Clock = std::chrono::steady_clock;
        using Duration = Clock::duration;
        using TimePoint = Clock::time_point;

    private:
        [[nodiscard]] static std::shared_ptr<detail::RunLoopState> lock_state_(
            const std::weak_ptr<detail::RunLoopState>& state) {
            const auto lockedState = state.lock();

            if (!lockedState) {
                throw std::logic_error {
                    "scheduler's run loop no longer exists"
                };
            }

            return lockedState;
        }

        class ScheduleAwaiter final {
        private:
            std::weak_ptr<detail::RunLoopState> state_ {};

        public:
            explicit ScheduleAwaiter(
                std::weak_ptr<detail::RunLoopState> state) noexcept
                : state_ { std::move(state) } {}

            [[nodiscard]] constexpr bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(std::coroutine_handle<> continuation) {
                // 入队后协程可能立即恢复，因此先把状态保存到当前线程的栈上。
                const auto state = Scheduler::lock_state_(state_);
                state->enqueue(continuation);
            }

            constexpr void await_resume() const noexcept {}
        };

        class ScheduleAfterAwaiter final {
        private:
            std::weak_ptr<detail::RunLoopState> state_ {};
            Duration delay_ {};

        public:
            ScheduleAfterAwaiter(
                std::weak_ptr<detail::RunLoopState> state,
                Duration delay) noexcept
                : state_ { std::move(state) }, delay_ { delay } {}

            [[nodiscard]] constexpr bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(std::coroutine_handle<> continuation) {
                // 入队后 awaiter 可能被销毁，先复制所有仍需使用的状态。
                const auto state = Scheduler::lock_state_(state_);
                const auto delay = delay_;
                state->enqueue_after(delay, continuation);
            }

            constexpr void await_resume() const noexcept {}
        };

        class ScheduleAtAwaiter final {
        private:
            std::weak_ptr<detail::RunLoopState> state_ {};
            TimePoint deadline_ {};

        public:
            ScheduleAtAwaiter(
                std::weak_ptr<detail::RunLoopState> state,
                TimePoint deadline) noexcept
                : state_ { std::move(state) }, deadline_ { deadline } {}

            [[nodiscard]] constexpr bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(std::coroutine_handle<> continuation) {
                const auto state = Scheduler::lock_state_(state_);
                const auto deadline = deadline_;
                state->enqueue_at(deadline, continuation);
            }

            constexpr void await_resume() const noexcept {}
        };

        std::weak_ptr<detail::RunLoopState> state_ {};

        explicit Scheduler(
            const std::shared_ptr<detail::RunLoopState>& state) noexcept
            : state_ { state } {}

        friend class RunLoop;

    public:
        Scheduler() = delete;
        Scheduler(const Scheduler&) = default;
        Scheduler& operator=(const Scheduler&) = default;
        Scheduler(Scheduler&&) noexcept = default;
        Scheduler& operator=(Scheduler&&) noexcept = default;
        ~Scheduler() = default;

        [[nodiscard]] auto schedule() const noexcept {
            return ScheduleAwaiter { state_ };
        }

        [[nodiscard]] auto schedule_after(Duration delay) const noexcept {
            return ScheduleAfterAwaiter { state_, delay };
        }

        [[nodiscard]] auto schedule_at(TimePoint deadline) const noexcept {
            return ScheduleAtAwaiter { state_, deadline };
        }

        friend bool operator==(
            const Scheduler& left,
            const Scheduler& right) noexcept {
            return !left.state_.owner_before(right.state_) &&
                !right.state_.owner_before(left.state_);
        }
    };

private:
    std::shared_ptr<detail::RunLoopState> state_ {
        std::make_shared<detail::RunLoopState>()
    };

public:
    RunLoop() = default;
    RunLoop(const RunLoop&) = delete;
    RunLoop& operator=(const RunLoop&) = delete;
    RunLoop(RunLoop&&) = delete;
    RunLoop& operator=(RunLoop&&) = delete;
    ~RunLoop() = default;

    [[nodiscard]] Scheduler get_scheduler() const noexcept {
        return Scheduler { state_ };
    }

    template<typename T>
    T run(Task<T> task) {
        detail::RootCompletion<T> completion {};
        detail::RunGuard guard { state_ };
        auto operation = detail::make_root_operation(
            std::move(task),
            completion);

        operation.bind(*state_, completion);
        state_->enqueue(operation.handle());
        state_->drive(completion);
        guard.release();

        if (completion.exception_) {
            std::rethrow_exception(completion.exception_);
        }

        if constexpr (!std::same_as<T, void>) {
            if (!completion.result_) {
                throw std::logic_error { "root task completed without a result" };
            }

            return std::move(*completion.result_);
        }
    }
};

}  // namespace mcpplibs::cmp
