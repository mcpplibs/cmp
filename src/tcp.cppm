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
    void ensure_accepting() {
        const std::lock_guard lock { admissionMutex_ };

        if (!accepting_) {
            throw std::logic_error { "I/O context is stopping" };
        }
    }

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
    std::atomic<bool> readPending_ { false };
    std::atomic<bool> writePending_ { false };
    std::mutex readTerminalMutex_ {};
    std::optional<std::error_code> readTerminal_ {};

public:
    explicit TcpSocketState(
        const std::shared_ptr<IoContextState>& context)
        : context_ { context },
          socket_ { std::in_place, context->ioContext_ } {}

    [[nodiscard]] asio::ip::tcp::socket& socket_on_driver() {
        if (!socket_) {
            throw std::logic_error { "TCP stream is closed" };
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

    [[nodiscard]] std::shared_ptr<IoContextState> lock_context() const {
        const auto context = context_.lock();

        if (!context) {
            throw std::logic_error { "I/O context no longer exists" };
        }

        return context;
    }

    void require_open() const {
        if (!logicallyOpen_.load(std::memory_order_acquire)) {
            throw std::logic_error { "TCP stream is closed" };
        }
    }

    [[nodiscard]] bool read_available() {
        const std::lock_guard lock { readTerminalMutex_ };

        if (logicallyOpen_.load(std::memory_order_acquire)) {
            return true;
        }

        // fatal read 与字节同时完成时，关闭后仍需交付一次 retained error。
        return readTerminal_ && *readTerminal_ != asio::error::eof;
    }

    [[nodiscard]] bool try_begin_read() noexcept {
        bool expected { false };
        return readPending_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel);
    }

    void finish_read() noexcept {
        if (!readPending_.exchange(false, std::memory_order_release)) {
            std::terminate();
        }
    }

    [[nodiscard]] bool try_begin_write() noexcept {
        bool expected { false };
        return writePending_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel);
    }

    void finish_write() noexcept {
        if (!writePending_.exchange(false, std::memory_order_release)) {
            std::terminate();
        }
    }

    [[nodiscard]] std::optional<std::error_code> take_read_terminal() {
        const std::lock_guard lock { readTerminalMutex_ };
        const auto terminal = readTerminal_;

        if (terminal && *terminal != asio::error::eof) {
            readTerminal_.reset();
        }

        return terminal;
    }

    void retain_read_terminal(std::error_code error) {
        if (!error) {
            std::terminate();
        }

        const std::lock_guard lock { readTerminalMutex_ };

        if (readTerminal_) {
            std::terminate();
        }

        readTerminal_ = error;
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

    template<typename ReturnScheduler>
    [[nodiscard]] static Task<std::size_t> read_some_impl_(
        std::shared_ptr<detail::TcpSocketState> state,
        ReturnScheduler returnTo,
        std::span<std::byte> buffer,
        std::stop_token stopToken);

    template<typename ReturnScheduler>
    [[nodiscard]] static Task<void> write_all_impl_(
        std::shared_ptr<detail::TcpSocketState> state,
        ReturnScheduler returnTo,
        std::span<const std::byte> buffer,
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

    template<typename ReturnScheduler>
    requires (
        std::move_constructible<ReturnScheduler> &&
        requires(const ReturnScheduler& scheduler) {
            scheduler.schedule();
        }
    )
    [[nodiscard]] Task<std::size_t> read_some(
        ReturnScheduler returnTo,
        std::span<std::byte> buffer,
        std::stop_token stopToken = {});

    template<typename ReturnScheduler>
    requires (
        std::move_constructible<ReturnScheduler> &&
        requires(const ReturnScheduler& scheduler) {
            scheduler.schedule();
        }
    )
    [[nodiscard]] Task<void> write_all(
        ReturnScheduler returnTo,
        std::span<const std::byte> buffer,
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
Task<std::size_t> TcpStream::read_some_impl_(
    std::shared_ptr<detail::TcpSocketState> state,
    ReturnScheduler returnTo,
    std::span<std::byte> buffer,
    std::stop_token stopToken) {
    std::optional<std::size_t> transferred {};
    std::exception_ptr exception {};
    bool admitted { false };
    const auto releaseAdmission = [&] noexcept {
        if (std::exchange(admitted, false)) {
            state->finish_read();
        }
    };

    try {
        if (!state) {
            throw std::logic_error { "TCP stream was moved from" };
        }

        const auto context = state->lock_context();
        context->ensure_accepting();

        if (!state->read_available()) {
            throw std::logic_error { "TCP stream is closed" };
        }

        if (!state->try_begin_read()) {
            throw std::logic_error { "TCP stream already has a pending read" };
        }

        admitted = true;

        if (const auto terminal = state->take_read_terminal()) {
            if (*terminal == asio::error::eof) {
                transferred = 0;
            } else {
                throw std::system_error { *terminal };
            }
        } else {
            state->require_open();

            if (stopToken.stop_requested()) {
                throw OperationCancelled {};
            }

            if (buffer.empty()) {
                transferred = 0;
            } else {
                const auto native = co_await detail::NativeOperationAwaiter {
                    context,
                    stopToken,
                    state,
                    [state, buffer](auto completion) mutable {
                        state->socket_on_driver().async_read_some(
                            asio::buffer(
                                buffer.data(),
                                buffer.size_bytes()),
                            std::move(completion));
                    }
                };

                if (native.transferred_ > buffer.size_bytes()) {
                    std::terminate();
                }

                if (!native.error_) {
                    transferred = native.transferred_;

                    if (native.transferred_ == 0) {
                        state->retain_read_terminal(asio::error::eof);
                    }
                } else if (native.transferred_ > 0) {
                    transferred = native.transferred_;

                    if (native.error_ == asio::error::eof) {
                        state->retain_read_terminal(native.error_);
                    } else if (native.cancellation_ ==
                        detail::NativeCancellationOrigin::none) {
                        state->retain_read_terminal(native.error_);
                        state->close_on_driver();
                    }
                } else if (native.cancellation_ !=
                    detail::NativeCancellationOrigin::none) {
                    throw OperationCancelled {};
                } else if (native.error_ == asio::error::eof) {
                    state->retain_read_terminal(native.error_);
                    transferred = 0;
                } else {
                    state->close_on_driver();
                    throw std::system_error { native.error_ };
                }
            }
        }
    } catch (...) {
        exception = std::current_exception();
    }

    try {
        co_await returnTo.schedule();
    } catch (...) {
        releaseAdmission();
        throw;
    }

    releaseAdmission();

    if (exception) {
        std::rethrow_exception(exception);
    }

    co_return *transferred;
}

template<typename ReturnScheduler>
Task<void> TcpStream::write_all_impl_(
    std::shared_ptr<detail::TcpSocketState> state,
    ReturnScheduler returnTo,
    std::span<const std::byte> buffer,
    std::stop_token stopToken) {
    std::exception_ptr exception {};
    bool admitted { false };
    const auto releaseAdmission = [&] noexcept {
        if (std::exchange(admitted, false)) {
            state->finish_write();
        }
    };

    try {
        if (!state) {
            throw std::logic_error { "TCP stream was moved from" };
        }

        const auto context = state->lock_context();
        context->ensure_accepting();
        state->require_open();

        if (!state->try_begin_write()) {
            throw std::logic_error { "TCP stream already has a pending write" };
        }

        admitted = true;

        if (stopToken.stop_requested()) {
            throw OperationCancelled {};
        }

        if (!buffer.empty()) {
            const auto native = co_await detail::NativeOperationAwaiter {
                context,
                stopToken,
                state,
                [state, buffer](auto completion) mutable {
                    asio::async_write(
                        state->socket_on_driver(),
                        asio::buffer(
                            buffer.data(),
                            buffer.size_bytes()),
                        std::move(completion));
                }
            };

            if (native.error_) {
                state->close_on_driver();

                if (native.cancellation_ !=
                    detail::NativeCancellationOrigin::none) {
                    throw OperationCancelled {};
                }

                throw std::system_error { native.error_ };
            }

            if (native.transferred_ != buffer.size_bytes()) {
                state->close_on_driver();
                std::terminate();
            }
        }
    } catch (...) {
        exception = std::current_exception();
    }

    try {
        co_await returnTo.schedule();
    } catch (...) {
        releaseAdmission();
        throw;
    }

    releaseAdmission();

    if (exception) {
        std::rethrow_exception(exception);
    }
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

template<typename ReturnScheduler>
requires (
    std::move_constructible<ReturnScheduler> &&
    requires(const ReturnScheduler& scheduler) {
        scheduler.schedule();
    }
)
Task<std::size_t> TcpStream::read_some(
    ReturnScheduler returnTo,
    std::span<std::byte> buffer,
    std::stop_token stopToken) {
    return read_some_impl_(
        state_,
        std::move(returnTo),
        buffer,
        std::move(stopToken));
}

template<typename ReturnScheduler>
requires (
    std::move_constructible<ReturnScheduler> &&
    requires(const ReturnScheduler& scheduler) {
        scheduler.schedule();
    }
)
Task<void> TcpStream::write_all(
    ReturnScheduler returnTo,
    std::span<const std::byte> buffer,
    std::stop_token stopToken) {
    return write_all_impl_(
        state_,
        std::move(returnTo),
        buffer,
        std::move(stopToken));
}

}  // namespace mcpplibs::cmp
