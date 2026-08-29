export module mcpplibs.cmp:tcp;

import std;
import asio;
import :cancellation;
import :task;

namespace mcpplibs::cmp::detail {

class TcpSocketState;

class IoContextState final {
private:
    using WorkGuard = decltype(asio::make_work_guard(
        std::declval<asio::io_context&>()));

    asio::io_context ioContext_ {};
    WorkGuard workGuard_ { asio::make_work_guard(ioContext_) };
    std::mutex admissionMutex_ {};
    bool accepting_ { true };
    // ponytail: v1 线性清理弱引用；实测注册或关闭成本显著后再换索引结构。
    std::vector<std::weak_ptr<TcpSocketState>> sockets_ {};

    void shutdown_on_driver_() noexcept;

    friend class TcpSocketState;

public:
    template<typename Handler>
    void post(Handler&& handler) {
        const std::lock_guard lock { admissionMutex_ };

        if (!accepting_) {
            throw std::logic_error { "I/O context is stopping" };
        }

        asio::post(ioContext_, std::forward<Handler>(handler));
    }

    void register_socket_on_driver(
        const std::shared_ptr<TcpSocketState>& socket) {
        std::erase_if(sockets_, [](const auto& registered) {
            return registered.expired();
        });
        sockets_.push_back(socket);
    }

    void run_driver() noexcept {
        try {
            ioContext_.run();
        } catch (...) {
            std::terminate();
        }
    }

    void begin_shutdown() noexcept {
        const std::lock_guard lock { admissionMutex_ };
        accepting_ = false;
        asio::post(ioContext_, [this] {
            shutdown_on_driver_();
        });
    }
};

class TcpSocketState final {
private:
    std::weak_ptr<IoContextState> context_ {};
    std::optional<asio::ip::tcp::socket> socket_ {};
    std::atomic<bool> logicallyOpen_ { false };

public:
    explicit TcpSocketState(
        const std::shared_ptr<IoContextState>& context)
        : context_ { context },
          socket_ { std::in_place, context->ioContext_ } {}

    [[nodiscard]] asio::ip::tcp::socket& socket_on_driver() noexcept {
        if (!socket_) {
            std::terminate();
        }

        return *socket_;
    }

    void mark_open_on_driver() noexcept {
        if (!socket_ || logicallyOpen_.exchange(
                true,
                std::memory_order_release)) {
            std::terminate();
        }
    }

    void close_on_driver() noexcept {
        logicallyOpen_.store(false, std::memory_order_release);

        if (!socket_) {
            return;
        }

        std::error_code ignored {};
        socket_->close(ignored);
        socket_.reset();
    }

    void request_close(
        const std::shared_ptr<TcpSocketState>& self) noexcept {
        if (!logicallyOpen_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        const auto context = context_.lock();

        if (!context) {
            return;
        }

        try {
            context->post([self] noexcept {
                self->close_on_driver();
            });
        } catch (const std::logic_error&) {
            // Shutdown 已接管 registry 中的 socket。
        } catch (...) {
            std::terminate();
        }
    }
};

void IoContextState::shutdown_on_driver_() noexcept {
    for (auto& registered : sockets_) {
        if (const auto socket = registered.lock()) {
            socket->close_on_driver();
        }
    }

    sockets_.clear();
    workGuard_.reset();
}

enum class NativeCancellationOrigin {
    none,
    stop_token,
    stream_close,
    context_shutdown
};

struct NativeOperationState final {
    struct Result final {
        std::error_code error_ {};
        std::size_t transferred_ {};
        NativeCancellationOrigin cancellation_ {
            NativeCancellationOrigin::none
        };
    };

    struct StopRequest final {
        std::weak_ptr<NativeOperationState> operation_ {};
        std::weak_ptr<IoContextState> context_ {};

        void operator()() const noexcept;
    };

    struct CompletionHandler final {
        std::shared_ptr<NativeOperationState> operation_ {};

        void operator()(std::error_code error) const noexcept;

        void operator()(
            std::error_code error,
            std::size_t transferred) const noexcept;
    };

    using StopCallback = std::stop_callback<StopRequest>;

    std::coroutine_handle<> continuation_ {};
    asio::cancellation_signal cancellationSignal_ {};
    std::error_code error_ {};
    std::size_t transferred_ {};
    NativeCancellationOrigin cancellation_ {
        NativeCancellationOrigin::none
    };
    std::exception_ptr exception_ {};
    bool completed_ { false };
    // 保证 native handler 返回前，connect/read/write 使用的资源仍然有效。
    std::shared_ptr<void> lifetime_ {};
    // 最先释放，避免停止回调继续访问即将销毁的 operation state。
    std::optional<StopCallback> stopCallback_ {};

    void complete_on_driver(
        std::error_code error,
        std::size_t transferred) noexcept {
        if (std::exchange(completed_, true)) {
            std::terminate();
        }

        error_ = error;
        transferred_ = transferred;
        resume_on_driver_();
    }

    void fail_initiation_on_driver(std::exception_ptr exception) noexcept {
        if (!exception || std::exchange(completed_, true)) {
            std::terminate();
        }

        exception_ = std::move(exception);
        resume_on_driver_();
    }

    void request_cancellation_on_driver(
        NativeCancellationOrigin origin) noexcept {
        if (completed_ || cancellation_ != NativeCancellationOrigin::none) {
            return;
        }

        cancellation_ = origin;
        cancellationSignal_.emit(asio::cancellation_type::all);
    }

    [[nodiscard]] Result take_result() {
        if (!completed_) {
            std::terminate();
        }

        stopCallback_.reset();

        if (exception_) {
            std::rethrow_exception(exception_);
        }

        return Result {
            error_,
            transferred_,
            cancellation_
        };
    }

private:
    void resume_on_driver_() noexcept {
        const auto continuation = std::exchange(continuation_, {});

        if (!continuation || continuation.done()) {
            std::terminate();
        }

        continuation.resume();
    }
};

void NativeOperationState::StopRequest::operator()() const noexcept {
    const auto operation = operation_.lock();
    const auto context = context_.lock();

    if (!operation || !context) {
        return;
    }

    try {
        context->post([operation] noexcept {
            operation->request_cancellation_on_driver(
                NativeCancellationOrigin::stop_token);
        });
    } catch (const std::logic_error&) {
        // Context shutdown 会关闭 socket，并由 native handler 完成该 operation。
    } catch (...) {
        std::terminate();
    }
}

void NativeOperationState::CompletionHandler::operator()(
    std::error_code error) const noexcept {
    operation_->complete_on_driver(error, 0);
}

void NativeOperationState::CompletionHandler::operator()(
    std::error_code error,
    std::size_t transferred) const noexcept {
    operation_->complete_on_driver(error, transferred);
}

template<typename Initiation>
class NativeOperationAwaiter final {
private:
    std::shared_ptr<IoContextState> context_ {};
    std::stop_token stopToken_ {};
    Initiation initiation_;
    std::shared_ptr<NativeOperationState> operation_ {
        std::make_shared<NativeOperationState>()
    };

public:
    NativeOperationAwaiter(
        std::shared_ptr<IoContextState> context,
        std::stop_token stopToken,
        std::shared_ptr<void> lifetime,
        Initiation initiation)
        : context_ { std::move(context) },
          stopToken_ { std::move(stopToken) },
          initiation_ { std::move(initiation) } {
        operation_->lifetime_ = std::move(lifetime);
    }

    [[nodiscard]] constexpr bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) {
        auto context = context_;
        auto operation = operation_;
        auto stopToken = stopToken_;
        auto initiation = std::move(initiation_);

        operation->continuation_ = continuation;

        // 发布后 continuation 可并发恢复，不再访问 awaiter 成员。
        context->post([
            context = std::move(context),
            operation = std::move(operation),
            stopToken = std::move(stopToken),
            initiation = std::move(initiation)
        ]() mutable noexcept {
            try {
                auto completion = asio::bind_cancellation_slot(
                    operation->cancellationSignal_.slot(),
                    NativeOperationState::CompletionHandler { operation });
                std::invoke(
                    std::move(initiation),
                    std::move(completion));
            } catch (...) {
                operation->fail_initiation_on_driver(
                    std::current_exception());
                return;
            }

            // initiation 成功后，只有 native handler 可以恢复 continuation。
            operation->stopCallback_.emplace(
                std::move(stopToken),
                NativeOperationState::StopRequest {
                    operation,
                    context
                });
        });
    }

    [[nodiscard]] NativeOperationState::Result await_resume() {
        return operation_->take_result();
    }
};

struct ConnectOperationState final {
    std::shared_ptr<TcpSocketState> socket_ {};
};

static_assert(std::invocable<
    NativeOperationState::CompletionHandler,
    std::error_code>);
static_assert(std::invocable<
    NativeOperationState::CompletionHandler,
    std::error_code,
    std::size_t>);
static_assert(std::is_nothrow_constructible_v<
    NativeOperationState::StopCallback,
    std::stop_token,
    NativeOperationState::StopRequest>);

}  // namespace mcpplibs::cmp::detail

export namespace mcpplibs::cmp {

class TcpStream;

class IoContext final {
private:
    // 共享状态必须先构造、后析构，driver 才能安全排空并退出。
    std::shared_ptr<detail::IoContextState> state_ {
        std::make_shared<detail::IoContextState>()
    };
    std::jthread driver_ { [state = state_] {
        state->run_driver();
    } };

    friend class TcpStream;

public:
    IoContext() = default;

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    ~IoContext() {
        if (driver_.get_id() == std::this_thread::get_id()) {
            std::terminate();
        }

        state_->begin_shutdown();
        driver_.join();
    }
};

class TcpStream final {
private:
    std::shared_ptr<detail::TcpSocketState> state_ {};

    explicit TcpStream(
        std::shared_ptr<detail::TcpSocketState> state) noexcept
        : state_ { std::move(state) } {}

    template<typename ReturnScheduler>
    [[nodiscard]] static Task<TcpStream> connect_impl_(
        std::weak_ptr<detail::IoContextState> context,
        ReturnScheduler returnTo,
        std::string numericAddress,
        std::uint16_t port,
        std::stop_token stopToken);

public:
    TcpStream() = delete;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;

    TcpStream(TcpStream&& other) noexcept
        : state_ { std::exchange(other.state_, {}) } {}

    TcpStream& operator=(TcpStream&&) = delete;

    ~TcpStream() {
        if (state_) {
            state_->request_close(state_);
        }
    }

    template<typename ReturnScheduler>
    requires (
        std::move_constructible<ReturnScheduler> &&
        requires(const ReturnScheduler& scheduler) {
            scheduler.schedule();
        }
    )
    [[nodiscard]] static Task<TcpStream> connect(
        IoContext& context,
        ReturnScheduler returnTo,
        std::string_view numericAddress,
        std::uint16_t port,
        std::stop_token stopToken = {});
};

template<typename ReturnScheduler>
Task<TcpStream> TcpStream::connect_impl_(
    std::weak_ptr<detail::IoContextState> weakContext,
    ReturnScheduler returnTo,
    std::string numericAddress,
    std::uint16_t port,
    std::stop_token stopToken) {
    auto connectState = std::make_shared<detail::ConnectOperationState>();
    std::optional<TcpStream> stream {};
    std::exception_ptr exception {};

    try {
        const auto context = weakContext.lock();

        if (!context) {
            throw std::logic_error { "I/O context no longer exists" };
        }

        const auto result = co_await detail::NativeOperationAwaiter {
            context,
            stopToken,
            connectState,
            [
                context,
                connectState,
                numericAddress = std::move(numericAddress),
                port,
                stopToken
            ](auto completion) mutable {
                if (stopToken.stop_requested()) {
                    throw OperationCancelled {};
                }

                std::error_code error {};
                const auto address = asio::ip::make_address(
                    numericAddress,
                    error);

                if (error) {
                    throw std::system_error { error };
                }

                auto socket = std::make_shared<detail::TcpSocketState>(
                    context);
                connectState->socket_ = socket;
                context->register_socket_on_driver(socket);
                socket->socket_on_driver().async_connect(
                    asio::ip::tcp::endpoint { address, port },
                    std::move(completion));
            }
        };

        if (result.error_) {
            if (result.cancellation_ !=
                detail::NativeCancellationOrigin::none) {
                throw OperationCancelled {};
            }

            throw std::system_error { result.error_ };
        }

        if (!connectState->socket_) {
            std::terminate();
        }

        connectState->socket_->mark_open_on_driver();
        stream.emplace(TcpStream { connectState->socket_ });
    } catch (...) {
        if (connectState->socket_) {
            connectState->socket_->close_on_driver();
        }

        exception = std::current_exception();
    }

    // 返回路径不接受取消，保证成功与错误都回到调用者选择的 Scheduler。
    co_await returnTo.schedule();

    if (exception) {
        std::rethrow_exception(exception);
    }

    co_return std::move(*stream);
}

template<typename ReturnScheduler>
requires (
    std::move_constructible<ReturnScheduler> &&
    requires(const ReturnScheduler& scheduler) {
        scheduler.schedule();
    }
)
Task<TcpStream> TcpStream::connect(
    IoContext& context,
    ReturnScheduler returnTo,
    std::string_view numericAddress,
    std::uint16_t port,
    std::stop_token stopToken) {
    return connect_impl_(
        context.state_,
        std::move(returnTo),
        std::string { numericAddress },
        port,
        std::move(stopToken));
}

}  // namespace mcpplibs::cmp
