export module mcpplibs.cmp:tcp;

import std;
import asio;
import :cancellation;
import :task;

namespace mcpplibs::cmp::detail {

class TcpSocketState;
class TcpListenerState;
struct NativeOperationState;

enum class NativeCancellationOrigin {
    none,
    stop_token,
    stream_close,
    listener_close,
    context_shutdown
};

class IoContextState final {
private:
    using WorkGuard = decltype(asio::make_work_guard(
        std::declval<asio::io_context&>()));

    asio::io_context ioContext_ {};
    WorkGuard workGuard_ { asio::make_work_guard(ioContext_) };
    std::mutex admissionMutex_ {};
    bool accepting_ { true };
    // ponytail: v1 线性清理两类弱引用；实测注册或关闭成本显著后再换索引结构。
    std::vector<std::weak_ptr<TcpSocketState>> sockets_ {};
    std::vector<std::weak_ptr<TcpListenerState>> listeners_ {};

    void shutdown_on_driver_() noexcept;

    friend class TcpSocketState;
    friend class TcpListenerState;

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

    void register_listener_on_driver(
        const std::shared_ptr<TcpListenerState>& listener) {
        std::erase_if(listeners_, [](const auto& registered) {
            return registered.expired();
        });
        listeners_.push_back(listener);
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
    std::atomic<bool> closeRequested_ { false };
    std::atomic<bool> readPending_ { false };
    std::atomic<bool> writePending_ { false };
    std::mutex readTerminalMutex_ {};
    std::optional<std::error_code> readTerminal_ {};
    std::weak_ptr<NativeOperationState> connectOperation_ {};
    std::weak_ptr<NativeOperationState> readOperation_ {};
    std::weak_ptr<NativeOperationState> writeOperation_ {};

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

    [[nodiscard]] bool try_mark_open_on_driver() noexcept {
        if (!socket_) {
            return false;
        }

        if (logicallyOpen_.exchange(true, std::memory_order_release)) {
            std::terminate();
        }

        return true;
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

    [[nodiscard]] bool is_open() const noexcept {
        return logicallyOpen_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool close_requested() const noexcept {
        return closeRequested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool read_available() {
        const std::lock_guard lock { readTerminalMutex_ };

        if (closeRequested_.load(std::memory_order_acquire)) {
            return false;
        }

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

    void attach_connect_operation_on_driver(
        const std::shared_ptr<NativeOperationState>& operation) noexcept;

    void attach_read_operation_on_driver(
        const std::shared_ptr<NativeOperationState>& operation) noexcept;

    void attach_write_operation_on_driver(
        const std::shared_ptr<NativeOperationState>& operation) noexcept;

    void cancel_and_close_on_driver(
        NativeCancellationOrigin origin) noexcept;

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
        closeRequested_.store(true, std::memory_order_release);

        if (!logicallyOpen_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        const auto context = context_.lock();

        if (!context) {
            return;
        }

        try {
            context->post([self] noexcept {
                self->cancel_and_close_on_driver(
                    NativeCancellationOrigin::stream_close);
            });
        } catch (const std::logic_error&) {
            // Shutdown 已接管 registry 中的 socket。
        } catch (...) {
            std::terminate();
        }
    }
};

class TcpListenerState final {
private:
    std::weak_ptr<IoContextState> context_ {};
    std::optional<asio::ip::tcp::acceptor> acceptor_ {};
    std::atomic<bool> logicallyOpen_ { false };
    std::atomic<bool> closeRequested_ { false };
    std::atomic<bool> acceptPending_ { false };
    std::atomic<std::uint16_t> localPort_ {};
    std::weak_ptr<NativeOperationState> acceptOperation_ {};

public:
    explicit TcpListenerState(
        const std::shared_ptr<IoContextState>& context)
        : context_ { context } {}

    void bind_on_driver(
        std::string_view numericAddress,
        std::uint16_t port) {
        std::error_code error {};
        const auto address = asio::ip::make_address(
            numericAddress,
            error);

        if (error) {
            throw std::system_error { error };
        }

        const auto context = lock_context();
        acceptor_.emplace(context->ioContext_);
        const asio::ip::tcp::endpoint endpoint { address, port };
        acceptor_->open(endpoint.protocol(), error);

        if (!error) {
            acceptor_->bind(endpoint, error);
        }

        if (!error) {
            acceptor_->listen(
                asio::socket_base::max_listen_connections,
                error);
        }

        std::uint16_t localPort {};

        if (!error) {
            localPort = acceptor_->local_endpoint(error).port();
        }

        if (error) {
            close_on_driver();
            throw std::system_error { error };
        }

        localPort_.store(localPort, std::memory_order_release);

        if (logicallyOpen_.exchange(true, std::memory_order_release)) {
            std::terminate();
        }
    }

    [[nodiscard]] asio::ip::tcp::acceptor& acceptor_on_driver() {
        if (!acceptor_) {
            throw std::logic_error { "TCP listener is closed" };
        }

        return *acceptor_;
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
            throw std::logic_error { "TCP listener is closed" };
        }
    }

    [[nodiscard]] bool is_open() const noexcept {
        return logicallyOpen_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool close_requested() const noexcept {
        return closeRequested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint16_t local_port() const noexcept {
        return localPort_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool try_begin_accept() noexcept {
        bool expected { false };
        return acceptPending_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel);
    }

    void finish_accept() noexcept {
        if (!acceptPending_.exchange(false, std::memory_order_release)) {
            std::terminate();
        }
    }

    void attach_accept_operation_on_driver(
        const std::shared_ptr<NativeOperationState>& operation) noexcept;

    void cancel_and_close_on_driver(
        NativeCancellationOrigin origin) noexcept;

    void close_on_driver() noexcept {
        logicallyOpen_.store(false, std::memory_order_release);

        if (!acceptor_) {
            return;
        }

        std::error_code ignored {};
        acceptor_->close(ignored);
        acceptor_.reset();
    }

    void request_close(
        const std::shared_ptr<TcpListenerState>& self) noexcept {
        closeRequested_.store(true, std::memory_order_release);

        if (!logicallyOpen_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        const auto context = context_.lock();

        if (!context) {
            return;
        }

        try {
            context->post([self] noexcept {
                self->cancel_and_close_on_driver(
                    NativeCancellationOrigin::listener_close);
            });
        } catch (const std::logic_error&) {
            // Shutdown 已接管 registry 中的 listener。
        } catch (...) {
            std::terminate();
        }
    }
};

void IoContextState::shutdown_on_driver_() noexcept {
    for (auto& registered : listeners_) {
        if (const auto listener = registered.lock()) {
            listener->cancel_and_close_on_driver(
                NativeCancellationOrigin::context_shutdown);
        }
    }

    listeners_.clear();

    for (auto& registered : sockets_) {
        if (const auto socket = registered.lock()) {
            socket->cancel_and_close_on_driver(
                NativeCancellationOrigin::context_shutdown);
        }
    }

    sockets_.clear();
    workGuard_.reset();
}

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
    // 保证 native handler 返回前，connect/accept/read/write 使用的资源仍然有效。
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
        if (origin == NativeCancellationOrigin::none) {
            std::terminate();
        }

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

void TcpSocketState::attach_connect_operation_on_driver(
    const std::shared_ptr<NativeOperationState>& operation) noexcept {
    if (!operation || !connectOperation_.expired()) {
        std::terminate();
    }

    connectOperation_ = operation;
}

void TcpSocketState::attach_read_operation_on_driver(
    const std::shared_ptr<NativeOperationState>& operation) noexcept {
    if (!operation) {
        std::terminate();
    }

    // readPending_ 已排除重叠；旧 Task 帧可能在完成后短暂存活。
    readOperation_ = operation;
}

void TcpSocketState::attach_write_operation_on_driver(
    const std::shared_ptr<NativeOperationState>& operation) noexcept {
    if (!operation) {
        std::terminate();
    }

    // writePending_ 已排除重叠；旧 Task 帧可能在完成后短暂存活。
    writeOperation_ = operation;
}

void TcpSocketState::cancel_and_close_on_driver(
    NativeCancellationOrigin origin) noexcept {
    const auto cancel = [origin](
        const std::weak_ptr<NativeOperationState>& operation) noexcept {
        if (const auto locked = operation.lock()) {
            locked->request_cancellation_on_driver(origin);
        }
    };

    cancel(connectOperation_);
    cancel(readOperation_);
    cancel(writeOperation_);
    close_on_driver();
}

void TcpListenerState::attach_accept_operation_on_driver(
    const std::shared_ptr<NativeOperationState>& operation) noexcept {
    if (!operation) {
        std::terminate();
    }

    // acceptPending_ 已排除重叠；旧 Task 帧可能在完成后短暂存活。
    acceptOperation_ = operation;
}

void TcpListenerState::cancel_and_close_on_driver(
    NativeCancellationOrigin origin) noexcept {
    if (const auto operation = acceptOperation_.lock()) {
        operation->request_cancellation_on_driver(origin);
    }

    close_on_driver();
}

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
                    operation,
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

struct DriverOperationState final {
    std::coroutine_handle<> continuation_ {};
    std::exception_ptr exception_ {};
    bool completed_ { false };

    void complete_on_driver(std::exception_ptr exception = {}) noexcept {
        if (std::exchange(completed_, true)) {
            std::terminate();
        }

        exception_ = std::move(exception);
        const auto continuation = std::exchange(continuation_, {});

        if (!continuation || continuation.done()) {
            std::terminate();
        }

        continuation.resume();
    }

    void take_result() {
        if (!completed_) {
            std::terminate();
        }

        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }
};

template<typename Operation>
class DriverOperationAwaiter final {
private:
    std::shared_ptr<IoContextState> context_ {};
    Operation operation_;
    std::shared_ptr<DriverOperationState> state_ {
        std::make_shared<DriverOperationState>()
    };

public:
    DriverOperationAwaiter(
        std::shared_ptr<IoContextState> context,
        Operation operation)
        : context_ { std::move(context) },
          operation_ { std::move(operation) } {}

    [[nodiscard]] constexpr bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) {
        auto state = state_;
        auto operation = std::move(operation_);
        state->continuation_ = continuation;

        context_->post([
            state = std::move(state),
            operation = std::move(operation)
        ]() mutable noexcept {
            try {
                std::invoke(std::move(operation));
                state->complete_on_driver();
            } catch (...) {
                state->complete_on_driver(std::current_exception());
            }
        });
    }

    void await_resume() {
        state_->take_result();
    }
};

struct ConnectOperationState final {
    std::shared_ptr<TcpSocketState> socket_ {};
};

struct AcceptOperationState final {
    std::shared_ptr<TcpListenerState> listener_ {};
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
class TcpListener;

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
    friend class TcpListener;

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

    friend class TcpListener;

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
        close();
    }

    void close() noexcept {
        if (state_) {
            state_->request_close(state_);
        }
    }

    [[nodiscard]] bool is_open() const noexcept {
        return state_ && state_->is_open();
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

class TcpListener final {
private:
    std::shared_ptr<detail::TcpListenerState> state_ {};

    explicit TcpListener(
        std::shared_ptr<detail::TcpListenerState> state) noexcept
        : state_ { std::move(state) } {}

    template<typename ReturnScheduler>
    [[nodiscard]] static Task<TcpListener> bind_impl_(
        std::weak_ptr<detail::IoContextState> context,
        ReturnScheduler returnTo,
        std::string numericAddress,
        std::uint16_t port,
        std::stop_token stopToken);

    template<typename ReturnScheduler>
    [[nodiscard]] static Task<TcpStream> accept_impl_(
        std::shared_ptr<detail::TcpListenerState> state,
        ReturnScheduler returnTo,
        std::stop_token stopToken);

public:
    TcpListener() = delete;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    TcpListener(TcpListener&& other) noexcept
        : state_ { std::exchange(other.state_, {}) } {}

    TcpListener& operator=(TcpListener&&) = delete;

    ~TcpListener() {
        close();
    }

    void close() noexcept {
        if (state_) {
            state_->request_close(state_);
        }
    }

    [[nodiscard]] bool is_open() const noexcept {
        return state_ && state_->is_open();
    }

    [[nodiscard]] std::uint16_t local_port() const noexcept {
        return state_ ? state_->local_port() : 0;
    }

    template<typename ReturnScheduler>
    requires (
        std::move_constructible<ReturnScheduler> &&
        requires(const ReturnScheduler& scheduler) {
            scheduler.schedule();
        }
    )
    [[nodiscard]] static Task<TcpListener> bind(
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
    [[nodiscard]] Task<TcpStream> accept(
        ReturnScheduler returnTo,
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
            ](const auto& operation, auto completion) mutable {
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
                socket->attach_connect_operation_on_driver(operation);
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

        if (!connectState->socket_->try_mark_open_on_driver()) {
            throw OperationCancelled {};
        }

        stream.emplace(TcpStream { connectState->socket_ });
    } catch (...) {
        if (connectState->socket_) {
            connectState->socket_->close_on_driver();
        }

        exception = std::current_exception();
    }

    // 返回路径不接受取消，保证成功与错误都回到调用者选择的 Scheduler。
    co_await returnTo.schedule();

    // Context 可在返回跳转期间关闭尚未发布的资源。
    if (!exception && !stream->is_open()) {
        exception = std::make_exception_ptr(OperationCancelled {});
    }

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
                    [state, buffer](
                        const auto& operation,
                        auto completion) mutable {
                        state->attach_read_operation_on_driver(operation);

                        if (state->close_requested()) {
                            throw OperationCancelled {};
                        }

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
                [state, buffer](
                    const auto& operation,
                    auto completion) mutable {
                    state->attach_write_operation_on_driver(operation);

                    if (state->close_requested()) {
                        throw OperationCancelled {};
                    }

                    asio::async_write(
                        state->socket_on_driver(),
                        asio::buffer(
                            buffer.data(),
                            buffer.size_bytes()),
                        std::move(completion));
                }
            };

            if (native.error_) {
                if (native.cancellation_ !=
                    detail::NativeCancellationOrigin::none) {
                    state->cancel_and_close_on_driver(
                        native.cancellation_);
                    throw OperationCancelled {};
                }

                state->close_on_driver();
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
Task<TcpListener> TcpListener::bind_impl_(
    std::weak_ptr<detail::IoContextState> weakContext,
    ReturnScheduler returnTo,
    std::string numericAddress,
    std::uint16_t port,
    std::stop_token stopToken) {
    std::shared_ptr<detail::TcpListenerState> listenerState {};
    std::optional<TcpListener> listener {};
    std::exception_ptr exception {};

    try {
        const auto context = weakContext.lock();

        if (!context) {
            throw std::logic_error { "I/O context no longer exists" };
        }

        listenerState = std::make_shared<detail::TcpListenerState>(
            context);
        co_await detail::DriverOperationAwaiter {
            context,
            [
                context,
                listenerState,
                numericAddress = std::move(numericAddress),
                port,
                stopToken
            ]() mutable {
                if (stopToken.stop_requested()) {
                    throw OperationCancelled {};
                }

                try {
                    listenerState->bind_on_driver(
                        numericAddress,
                        port);
                    context->register_listener_on_driver(listenerState);
                } catch (...) {
                    listenerState->close_on_driver();
                    throw;
                }
            }
        };
        listener.emplace(TcpListener { listenerState });
    } catch (...) {
        exception = std::current_exception();
    }

    // 返回路径不接受取消，保证成功与错误都回到调用者选择的 Scheduler。
    co_await returnTo.schedule();

    // Context 可在返回跳转期间关闭尚未发布的资源。
    if (!exception && !listener->is_open()) {
        exception = std::make_exception_ptr(OperationCancelled {});
    }

    if (exception) {
        std::rethrow_exception(exception);
    }

    co_return std::move(*listener);
}

template<typename ReturnScheduler>
Task<TcpStream> TcpListener::accept_impl_(
    std::shared_ptr<detail::TcpListenerState> state,
    ReturnScheduler returnTo,
    std::stop_token stopToken) {
    std::shared_ptr<detail::AcceptOperationState> acceptState {};
    std::optional<TcpStream> stream {};
    std::exception_ptr exception {};
    bool admitted { false };
    const auto releaseAdmission = [&] noexcept {
        if (std::exchange(admitted, false)) {
            state->finish_accept();
        }
    };

    try {
        if (!state) {
            throw std::logic_error { "TCP listener was moved from" };
        }

        const auto context = state->lock_context();
        context->ensure_accepting();
        state->require_open();

        if (!state->try_begin_accept()) {
            throw std::logic_error {
                "TCP listener already has a pending accept"
            };
        }

        admitted = true;

        if (stopToken.stop_requested()) {
            throw OperationCancelled {};
        }

        acceptState = std::make_shared<detail::AcceptOperationState>();
        acceptState->listener_ = state;
        const auto native = co_await detail::NativeOperationAwaiter {
            context,
            stopToken,
            acceptState,
            [context, acceptState, stopToken](
                const auto& operation,
                auto completion) mutable {
                if (stopToken.stop_requested()) {
                    throw OperationCancelled {};
                }

                auto socket = std::make_shared<detail::TcpSocketState>(
                    context);
                acceptState->socket_ = socket;
                // 先登记候选 socket，确保 context shutdown 不会漏掉已选连接。
                context->register_socket_on_driver(socket);
                acceptState->listener_->attach_accept_operation_on_driver(
                    operation);

                if (acceptState->listener_->close_requested()) {
                    throw OperationCancelled {};
                }

                acceptState->listener_->acceptor_on_driver().async_accept(
                    socket->socket_on_driver(),
                    std::move(completion));
            }
        };

        if (native.error_) {
            if (native.cancellation_ !=
                detail::NativeCancellationOrigin::none) {
                throw OperationCancelled {};
            }

            throw std::system_error { native.error_ };
        }

        if (!acceptState->socket_) {
            std::terminate();
        }

        if (!acceptState->socket_->try_mark_open_on_driver()) {
            throw OperationCancelled {};
        }

        stream.emplace(TcpStream { acceptState->socket_ });
    } catch (...) {
        if (acceptState && acceptState->socket_) {
            acceptState->socket_->close_on_driver();
        }

        exception = std::current_exception();
    }

    try {
        co_await returnTo.schedule();
    } catch (...) {
        releaseAdmission();
        throw;
    }

    // Context 可在返回跳转期间关闭尚未发布的资源。
    if (!exception && !stream->is_open()) {
        exception = std::make_exception_ptr(OperationCancelled {});
    }

    releaseAdmission();

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

template<typename ReturnScheduler>
requires (
    std::move_constructible<ReturnScheduler> &&
    requires(const ReturnScheduler& scheduler) {
        scheduler.schedule();
    }
)
Task<TcpListener> TcpListener::bind(
    IoContext& context,
    ReturnScheduler returnTo,
    std::string_view numericAddress,
    std::uint16_t port,
    std::stop_token stopToken) {
    return bind_impl_(
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
Task<TcpStream> TcpListener::accept(
    ReturnScheduler returnTo,
    std::stop_token stopToken) {
    return accept_impl_(
        state_,
        std::move(returnTo),
        std::move(stopToken));
}

}  // namespace mcpplibs::cmp
