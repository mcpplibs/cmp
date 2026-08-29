export module mcpplibs.cmp:async_manual_reset_event;

import std;
import :cancellation;

export namespace mcpplibs::cmp {

class AsyncManualResetEvent final {
private:
    enum class WaitOutcome {
        pending,
        signalled,
        cancelled
    };

public:
    class WaitOperation final {
    private:
        friend class AsyncManualResetEvent;

        struct CancelCallback final {
            WaitOperation* operation_ {};

            void operator()() const noexcept {
                operation_->request_cancel_();
            }
        };

        using StopCallback = std::stop_callback<CancelCallback>;

        AsyncManualResetEvent* event_ {};
        WaitOperation* previous_ {};
        WaitOperation* next_ {};
        WaitOperation* dispatchNext_ {};
        std::coroutine_handle<> continuation_ {};
        std::stop_token stopToken_ {};
        std::atomic<bool> stopRequested_ { false };
        WaitOutcome outcome_ { WaitOutcome::pending };
        bool queued_ { false };
        // 最先析构，阻止回调继续访问当前协程帧。
        std::optional<StopCallback> stopCallback_ {};

        WaitOperation(
            AsyncManualResetEvent& event,
            std::stop_token stopToken) noexcept
            : event_ { &event },
              stopToken_ { std::move(stopToken) } {}

        void request_cancel_() noexcept {
            auto* const event = event_;
            stopRequested_.store(true, std::memory_order_release);
            event->cancel_waiter_(*this);
        }

    public:
        WaitOperation(const WaitOperation&) = delete;
        WaitOperation& operator=(const WaitOperation&) = delete;
        WaitOperation(WaitOperation&&) = delete;
        WaitOperation& operator=(WaitOperation&&) = delete;

        [[nodiscard]] constexpr bool await_ready() const noexcept {
            return false;
        }

        [[nodiscard]] bool await_suspend(
            std::coroutine_handle<> continuation) {
            auto* const event = event_;
            continuation_ = continuation;

            if (stopToken_.stop_possible()) {
                stopCallback_.emplace(
                    stopToken_,
                    CancelCallback { this });
            }

            // 发布后 set() 或取消可能立即恢复并销毁当前 operation。
            return event->register_waiter_(*this);
        }

        void await_resume() {
            stopCallback_.reset();

            if (outcome_ == WaitOutcome::cancelled) {
                throw OperationCancelled {};
            }
        }
    };

private:
    // ponytail: 标准状态锁换取可审查的 reset/cancel 竞态；实测瓶颈后再考虑原子代际状态。
    mutable std::mutex mutex_ {};
    bool set_ { false };
    WaitOperation* head_ {};
    WaitOperation* tail_ {};

    inline static thread_local bool dispatching_ { false };
    inline static thread_local WaitOperation* dispatchHead_ {};
    inline static thread_local WaitOperation* dispatchTail_ {};

    [[nodiscard]] bool register_waiter_(WaitOperation& operation) noexcept;
    void cancel_waiter_(WaitOperation& operation) noexcept;
    static void dispatch_(WaitOperation* operations) noexcept;

public:
    explicit AsyncManualResetEvent(bool initiallySet = false) noexcept
        : set_ { initiallySet } {}

    AsyncManualResetEvent(const AsyncManualResetEvent&) = delete;
    AsyncManualResetEvent& operator=(const AsyncManualResetEvent&) = delete;
    AsyncManualResetEvent(AsyncManualResetEvent&&) = delete;
    AsyncManualResetEvent& operator=(AsyncManualResetEvent&&) = delete;
    ~AsyncManualResetEvent();

    [[nodiscard]] bool is_set() const noexcept {
        const std::lock_guard lock { mutex_ };
        return set_;
    }

    void set() noexcept;
    void reset() noexcept;

    [[nodiscard]] WaitOperation wait(
        std::stop_token stopToken = {}) noexcept {
        return WaitOperation { *this, std::move(stopToken) };
    }

    [[nodiscard]] WaitOperation operator co_await() noexcept {
        return wait();
    }
};

inline AsyncManualResetEvent::~AsyncManualResetEvent() {
    const std::lock_guard lock { mutex_ };

    if (head_ != nullptr || tail_ != nullptr) {
        std::terminate();
    }
}

inline bool AsyncManualResetEvent::register_waiter_(
    WaitOperation& operation) noexcept {
    const std::lock_guard lock { mutex_ };

    if (operation.stopRequested_.load(std::memory_order_acquire)) {
        operation.outcome_ = WaitOutcome::cancelled;
        return false;
    }

    if (set_) {
        operation.outcome_ = WaitOutcome::signalled;
        return false;
    }

    operation.previous_ = tail_;
    operation.next_ = nullptr;
    operation.queued_ = true;

    if (tail_) {
        tail_->next_ = &operation;
    } else {
        head_ = &operation;
    }

    tail_ = &operation;
    return true;
}

inline void AsyncManualResetEvent::cancel_waiter_(
    WaitOperation& operation) noexcept {
    {
        const std::lock_guard lock { mutex_ };

        if (!operation.queued_ ||
            operation.outcome_ != WaitOutcome::pending) {
            return;
        }

        if (operation.previous_) {
            operation.previous_->next_ = operation.next_;
        } else {
            head_ = operation.next_;
        }

        if (operation.next_) {
            operation.next_->previous_ = operation.previous_;
        } else {
            tail_ = operation.previous_;
        }

        operation.previous_ = nullptr;
        operation.next_ = nullptr;
        operation.queued_ = false;
        operation.outcome_ = WaitOutcome::cancelled;
    }

    dispatch_(&operation);
}

inline void AsyncManualResetEvent::dispatch_(
    WaitOperation* operations) noexcept {
    while (operations) {
        auto* const current = operations;
        operations = current->next_;
        current->previous_ = nullptr;
        current->next_ = nullptr;
        current->dispatchNext_ = nullptr;

        if (dispatchTail_) {
            dispatchTail_->dispatchNext_ = current;
        } else {
            dispatchHead_ = current;
        }
        dispatchTail_ = current;
    }

    if (dispatching_) {
        return;
    }

    dispatching_ = true;
    while (dispatchHead_) {
        auto* const current = dispatchHead_;
        dispatchHead_ = current->dispatchNext_;
        if (!dispatchHead_) {
            dispatchTail_ = nullptr;
        }

        // 恢复后 operation 可能销毁，循环不再访问 current。
        const auto continuation = current->continuation_;
        continuation.resume();
    }
    dispatching_ = false;
}

inline void AsyncManualResetEvent::set() noexcept {
    WaitOperation* waiters {};

    {
        const std::lock_guard lock { mutex_ };

        if (set_) {
            return;
        }

        set_ = true;
        waiters = std::exchange(head_, nullptr);
        tail_ = nullptr;

        for (auto* waiter = waiters; waiter; waiter = waiter->next_) {
            waiter->queued_ = false;
            waiter->outcome_ = WaitOutcome::signalled;
        }
    }

    dispatch_(waiters);
}

inline void AsyncManualResetEvent::reset() noexcept {
    const std::lock_guard lock { mutex_ };
    set_ = false;
}

}  // namespace mcpplibs::cmp
