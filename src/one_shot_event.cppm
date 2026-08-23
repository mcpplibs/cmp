export module mcpplibs.cmp:one_shot_event;

import std;

export namespace mcpplibs::cmp {

class OneShotEvent final {
public:
    class Awaiter final {
    private:
        friend class OneShotEvent;

        OneShotEvent* event_ {};
        Awaiter* next_ {};
        std::coroutine_handle<> continuation_ {};

        explicit Awaiter(OneShotEvent& event) noexcept
            : event_ { &event } {}

    public:
        Awaiter(const Awaiter&) = delete;
        Awaiter& operator=(const Awaiter&) = delete;
        Awaiter(Awaiter&&) = delete;
        Awaiter& operator=(Awaiter&&) = delete;

        [[nodiscard]] bool await_ready() const noexcept {
            return event_->is_set();
        }

        [[nodiscard]] bool await_suspend(
            std::coroutine_handle<> continuation) noexcept {
            // 注册成功后 set() 可能立即恢复并销毁当前 awaiter。
            auto* const event = event_;
            continuation_ = continuation;
            return event->register_waiter_(*this);
        }

        constexpr void await_resume() const noexcept {}
    };

private:
    std::atomic<void*> state_ {};

    [[nodiscard]] bool register_waiter_(Awaiter& waiter) noexcept;

public:
    OneShotEvent() noexcept = default;
    OneShotEvent(const OneShotEvent&) = delete;
    OneShotEvent& operator=(const OneShotEvent&) = delete;
    OneShotEvent(OneShotEvent&&) = delete;
    OneShotEvent& operator=(OneShotEvent&&) = delete;
    ~OneShotEvent();

    [[nodiscard]] bool is_set() const noexcept {
        return state_.load(std::memory_order_acquire) ==
            static_cast<const void*>(this);
    }

    void set() noexcept;

    [[nodiscard]] Awaiter operator co_await() noexcept {
        return Awaiter { *this };
    }
};

inline OneShotEvent::~OneShotEvent() {
    const auto state = state_.load(std::memory_order_relaxed);

    // 等待节点属于协程帧，事件不能在仍引用这些节点时销毁。
    if (state != nullptr && state != static_cast<void*>(this)) {
        std::terminate();
    }
}

inline bool OneShotEvent::register_waiter_(Awaiter& waiter) noexcept {
    const auto setState = static_cast<void*>(this);
    auto state = state_.load(std::memory_order_acquire);

    do {
        if (state == setState) {
            return false;
        }

        waiter.next_ = static_cast<Awaiter*>(state);
    } while (!state_.compare_exchange_weak(
        state,
        &waiter,
        std::memory_order_release,
        std::memory_order_acquire));

    return true;
}

inline void OneShotEvent::set() noexcept {
    const auto setState = static_cast<void*>(this);
    auto state = state_.exchange(setState, std::memory_order_acq_rel);

    if (state == setState) {
        return;
    }

    while (state != nullptr) {
        // 恢复后当前节点可能立即销毁，因此先复制后继和句柄。
        auto* const waiter = static_cast<Awaiter*>(state);
        auto* const next = waiter->next_;
        const auto continuation = waiter->continuation_;
        state = next;
        continuation.resume();
    }
}

}  // namespace mcpplibs::cmp
