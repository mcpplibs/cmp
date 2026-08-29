export module mcpplibs.cmp:thread_pool;

import std;
import :cancellation;

namespace mcpplibs::cmp::detail {

enum class ThreadPoolOutcome {
    pending,
    completed,
    cancelled
};

struct ThreadPoolCancellation final {
    std::atomic<ThreadPoolOutcome> outcome_ { ThreadPoolOutcome::pending };
};

struct ThreadPoolCancelCallback final {
    ThreadPoolCancellation* cancellation_ {};

    void operator()() const noexcept {
        auto expected = ThreadPoolOutcome::pending;
        cancellation_->outcome_.compare_exchange_strong(
            expected,
            ThreadPoolOutcome::cancelled,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
};

struct ThreadPoolEntry final {
    std::coroutine_handle<> continuation_ {};
    ThreadPoolCancellation* cancellation_ {};
};

class ThreadPoolState final {
private:
    std::mutex mutex_ {};
    std::condition_variable condition_ {};
    // ponytail: v1 使用共享 FIFO；基准证明锁竞争后再评审分片队列。
    std::deque<ThreadPoolEntry> ready_ {};
    bool accepting_ { true };

public:
    void enqueue(
        std::coroutine_handle<> continuation,
        ThreadPoolCancellation* cancellation = nullptr) {
        if (!continuation) {
            throw std::invalid_argument { "cannot schedule an empty coroutine" };
        }

        {
            const std::lock_guard lock { mutex_ };

            if (!accepting_) {
                throw std::logic_error { "thread pool is stopping" };
            }

            ready_.push_back(ThreadPoolEntry {
                continuation,
                cancellation
            });
        }

        condition_.notify_one();
    }

    void run_worker() noexcept {
        while (true) {
            ThreadPoolEntry entry {};

            {
                std::unique_lock lock { mutex_ };
                condition_.wait(lock, [&] {
                    return !ready_.empty() || !accepting_;
                });

                if (ready_.empty()) {
                    return;
                }

                entry = ready_.front();
                ready_.pop_front();

                if (entry.cancellation_) {
                    auto expected = ThreadPoolOutcome::pending;
                    entry.cancellation_->outcome_.compare_exchange_strong(
                        expected,
                        ThreadPoolOutcome::completed,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire);
                }
            }

            if (!entry.continuation_ || entry.continuation_.done()) {
                std::terminate();
            }

            // 锁外恢复，协程再次调度到当前池时不会自锁。
            entry.continuation_.resume();
        }
    }

    void close() noexcept {
        {
            const std::lock_guard lock { mutex_ };
            accepting_ = false;
        }

        condition_.notify_all();
    }
};

}  // namespace mcpplibs::cmp::detail

export namespace mcpplibs::cmp {

class ThreadPool final {
public:
    class Scheduler final {
    private:
        [[nodiscard]] static std::shared_ptr<detail::ThreadPoolState> lock_state_(
            const std::weak_ptr<detail::ThreadPoolState>& state) {
            const auto lockedState = state.lock();

            if (!lockedState) {
                throw std::logic_error {
                    "scheduler's thread pool no longer exists"
                };
            }

            return lockedState;
        }

        class ScheduleAwaiter final {
        private:
            std::weak_ptr<detail::ThreadPoolState> state_ {};

        public:
            explicit ScheduleAwaiter(
                std::weak_ptr<detail::ThreadPoolState> state) noexcept
                : state_ { std::move(state) } {}

            [[nodiscard]] constexpr bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(std::coroutine_handle<> continuation) {
                const auto state = Scheduler::lock_state_(state_);
                state->enqueue(continuation);
            }

            constexpr void await_resume() const noexcept {}
        };

        class CancellableScheduleAwaiter final {
        private:
            using StopCallback =
                std::stop_callback<detail::ThreadPoolCancelCallback>;

            std::weak_ptr<detail::ThreadPoolState> state_ {};
            std::stop_token stopToken_ {};
            detail::ThreadPoolCancellation cancellation_ {};
            // 最先析构，阻止回调继续访问 awaiter 内状态。
            std::optional<StopCallback> stopCallback_ {};

        public:
            CancellableScheduleAwaiter(
                std::weak_ptr<detail::ThreadPoolState> state,
                std::stop_token stopToken) noexcept
                : state_ { std::move(state) },
                  stopToken_ { std::move(stopToken) } {}

            [[nodiscard]] constexpr bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(std::coroutine_handle<> continuation) {
                const auto state = Scheduler::lock_state_(state_);

                stopCallback_.emplace(
                    stopToken_,
                    detail::ThreadPoolCancelCallback { &cancellation_ });

                // 发布后 awaiter 可能立即销毁，此后不得再读取成员。
                state->enqueue(continuation, &cancellation_);
            }

            void await_resume() {
                stopCallback_.reset();

                if (cancellation_.outcome_.load(
                        std::memory_order_acquire) ==
                    detail::ThreadPoolOutcome::cancelled) {
                    throw OperationCancelled {};
                }
            }
        };

        std::weak_ptr<detail::ThreadPoolState> state_ {};

        explicit Scheduler(
            const std::shared_ptr<detail::ThreadPoolState>& state) noexcept
            : state_ { state } {}

        friend class ThreadPool;

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

        [[nodiscard]] auto schedule(
            std::stop_token stopToken) const noexcept {
            return CancellableScheduleAwaiter {
                state_,
                std::move(stopToken)
            };
        }

        friend bool operator==(
            const Scheduler& left,
            const Scheduler& right) noexcept {
            return !left.state_.owner_before(right.state_) &&
                !right.state_.owner_before(left.state_);
        }
    };

private:
    std::shared_ptr<detail::ThreadPoolState> state_ {
        std::make_shared<detail::ThreadPoolState>()
    };
    // state_ 必须晚于 worker 析构，确保线程退出前共享状态仍然存在。
    std::vector<std::jthread> workers_ {};

    [[nodiscard]] static std::size_t default_worker_count_() noexcept {
        const auto count = std::thread::hardware_concurrency();
        return count == 0 ? 1 : static_cast<std::size_t>(count);
    }

public:
    ThreadPool()
        : ThreadPool { default_worker_count_() } {}

    explicit ThreadPool(std::size_t workerCount) {
        if (workerCount == 0) {
            throw std::invalid_argument {
                "thread pool requires at least one worker"
            };
        }

        workers_.reserve(workerCount);

        try {
            for (std::size_t index { 0 }; index < workerCount; ++index) {
                workers_.emplace_back([state = state_] {
                    state->run_worker();
                });
            }
        } catch (...) {
            // 先关闭并唤醒，随后 jthread 的成员析构会安全 join。
            state_->close();
            throw;
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    ~ThreadPool() {
        const auto currentThread = std::this_thread::get_id();

        if (std::ranges::any_of(workers_, [&](const std::jthread& worker) {
                return worker.get_id() == currentThread;
            })) {
            std::terminate();
        }

        // jthread 的 stop token 不参与关闭；worker 会先排空已接纳任务。
        state_->close();
    }

    [[nodiscard]] std::size_t thread_count() const noexcept {
        return workers_.size();
    }

    [[nodiscard]] Scheduler get_scheduler() const noexcept {
        return Scheduler { state_ };
    }
};

}  // namespace mcpplibs::cmp
