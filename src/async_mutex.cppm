export module mcpplibs.cmp:async_mutex;

import std;

export namespace mcpplibs::cmp {

class AsyncMutex final {
public:
    class LockOperation;

    class [[nodiscard]] Guard final {
    private:
        friend class AsyncMutex;
        friend class LockOperation;

        AsyncMutex* mutex_ {};

        explicit Guard(AsyncMutex& mutex) noexcept
            : mutex_ { &mutex } {}

    public:
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        Guard(Guard&& other) noexcept
            : mutex_ { std::exchange(other.mutex_, nullptr) } {}

        Guard& operator=(Guard&&) = delete;

        ~Guard() {
            if (mutex_) {
                mutex_->unlock_();
            }
        }
    };

    class LockOperation final {
    private:
        friend class AsyncMutex;

        AsyncMutex* mutex_ {};
        LockOperation* next_ {};
        LockOperation* dispatchNext_ {};
        std::coroutine_handle<> continuation_ {};

        explicit LockOperation(AsyncMutex& mutex) noexcept
            : mutex_ { &mutex } {}

    public:
        LockOperation(const LockOperation&) = delete;
        LockOperation& operator=(const LockOperation&) = delete;
        LockOperation(LockOperation&&) = delete;
        LockOperation& operator=(LockOperation&&) = delete;

        [[nodiscard]] constexpr bool await_ready() const noexcept {
            return false;
        }

        [[nodiscard]] bool await_suspend(
            std::coroutine_handle<> continuation) noexcept {
            // 入队后 Guard 可能立即交接所有权并销毁当前 operation。
            auto* const mutex = mutex_;
            continuation_ = continuation;
            return mutex->acquire_or_enqueue_(*this);
        }

        [[nodiscard]] Guard await_resume() const noexcept {
            return Guard { *mutex_ };
        }
    };

private:
    // ponytail: 单状态锁优先保证可审查的 FIFO；基准证明瓶颈后再换原子双队列。
    std::mutex stateMutex_ {};
    bool locked_ { false };
    LockOperation* head_ {};
    LockOperation* tail_ {};

    inline static thread_local bool dispatching_ { false };
    inline static thread_local LockOperation* dispatchHead_ {};
    inline static thread_local LockOperation* dispatchTail_ {};

    [[nodiscard]] bool acquire_or_enqueue_(LockOperation& operation) noexcept;
    void unlock_() noexcept;
    static void dispatch_(LockOperation& operation) noexcept;

public:
    AsyncMutex() = default;
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;
    ~AsyncMutex();

    [[nodiscard]] LockOperation lock_async() noexcept {
        return LockOperation { *this };
    }
};

inline AsyncMutex::~AsyncMutex() {
    const std::lock_guard lock { stateMutex_ };

    if (locked_ || head_ != nullptr || tail_ != nullptr) {
        std::terminate();
    }
}

inline bool AsyncMutex::acquire_or_enqueue_(
    LockOperation& operation) noexcept {
    const std::lock_guard lock { stateMutex_ };

    if (!locked_) {
        locked_ = true;
        return false;
    }

    operation.next_ = nullptr;
    if (tail_) {
        tail_->next_ = &operation;
    } else {
        head_ = &operation;
    }
    tail_ = &operation;
    return true;
}

inline void AsyncMutex::unlock_() noexcept {
    LockOperation* next {};

    {
        const std::lock_guard lock { stateMutex_ };

        if (!locked_) {
            std::terminate();
        }

        next = head_;
        if (next) {
            head_ = next->next_;
            if (!head_) {
                tail_ = nullptr;
            }
        } else {
            locked_ = false;
        }
    }

    if (next) {
        dispatch_(*next);
    }
}

inline void AsyncMutex::dispatch_(LockOperation& operation) noexcept {
    operation.dispatchNext_ = nullptr;
    if (dispatchTail_) {
        dispatchTail_->dispatchNext_ = &operation;
    } else {
        dispatchHead_ = &operation;
    }
    dispatchTail_ = &operation;

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

}  // namespace mcpplibs::cmp
