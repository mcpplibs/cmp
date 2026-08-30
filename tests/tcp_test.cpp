#include <gtest/gtest.h>

import std;
import asio;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::IoContext;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::TcpListener;
using mcpplibs::cmp::TcpStream;
using mcpplibs::cmp::ThreadPool;

using namespace std::chrono_literals;

static_assert(std::default_initializable<IoContext>);
static_assert(std::destructible<IoContext>);
static_assert(std::is_final_v<IoContext>);
static_assert(!std::copy_constructible<IoContext>);
static_assert(!std::move_constructible<IoContext>);
static_assert(!std::is_copy_assignable_v<IoContext>);
static_assert(!std::is_move_assignable_v<IoContext>);

static_assert(!std::default_initializable<TcpStream>);
static_assert(std::destructible<TcpStream>);
static_assert(std::is_final_v<TcpStream>);
static_assert(!std::copy_constructible<TcpStream>);
static_assert(std::move_constructible<TcpStream>);
static_assert(std::is_nothrow_move_constructible_v<TcpStream>);
static_assert(!std::is_copy_assignable_v<TcpStream>);
static_assert(!std::is_move_assignable_v<TcpStream>);
static_assert(noexcept(std::declval<TcpStream&>().close()));
static_assert(noexcept(std::declval<const TcpStream&>().is_open()));

static_assert(!std::default_initializable<TcpListener>);
static_assert(std::destructible<TcpListener>);
static_assert(std::is_final_v<TcpListener>);
static_assert(!std::copy_constructible<TcpListener>);
static_assert(std::move_constructible<TcpListener>);
static_assert(std::is_nothrow_move_constructible_v<TcpListener>);
static_assert(!std::is_copy_assignable_v<TcpListener>);
static_assert(!std::is_move_assignable_v<TcpListener>);
static_assert(noexcept(std::declval<TcpListener&>().close()));
static_assert(noexcept(std::declval<const TcpListener&>().is_open()));
static_assert(noexcept(std::declval<const TcpListener&>().local_port()));

class LoopbackServer final {
public:
    using Protocol = std::function<void(asio::ip::tcp::socket&)>;

private:
    asio::io_context ioContext_ {};
    asio::ip::tcp::acceptor acceptor_ { ioContext_ };
    asio::ip::tcp::endpoint endpoint_ {};
    std::error_code startError_ {};
    std::binary_semaphore ready_ { 0 };
    std::binary_semaphore acceptedSignal_ { 0 };
    std::latch release_ { 1 };
    std::atomic<bool> accepted_ { false };
    std::size_t connectionCount_ { 1 };
    std::atomic<std::size_t> remainingConnections_ { 1 };
    Protocol protocol_ {};
    std::jthread worker_ {};

    void run_() noexcept {
        ready_.release();

        // 服务端串行处理会话；客户端仍在同一 IoContext 上并发等待。
        for (std::size_t index {}; index < connectionCount_; ++index) {
            asio::ip::tcp::socket socket { ioContext_ };
            std::error_code error {};
            acceptor_.accept(socket, error);

            if (error) {
                return;
            }

            const auto remaining = remainingConnections_.fetch_sub(
                1,
                std::memory_order_acq_rel);

            if (remaining == 0) {
                std::terminate();
            }

            accepted_.store(true, std::memory_order_release);

            if (remaining == connectionCount_) {
                acceptedSignal_.release();
            }

            if (protocol_) {
                std::invoke(protocol_, socket);
            } else {
                release_.wait();
            }
        }
    }

public:
    explicit LoopbackServer(
        const asio::ip::address& address,
        Protocol protocol = {},
        std::size_t connectionCount = 1)
        : connectionCount_ { connectionCount },
          remainingConnections_ { connectionCount },
          protocol_ { std::move(protocol) } {
        const asio::ip::tcp::endpoint requested { address, 0 };
        acceptor_.open(requested.protocol(), startError_);

        if (startError_) {
            return;
        }

        // 重复测试会留下 TIME_WAIT；允许内核安全复用已关闭的 loopback 端口。
        acceptor_.set_option(
            asio::socket_base::reuse_address { true },
            startError_);

        if (startError_) {
            return;
        }

        acceptor_.bind(requested, startError_);

        if (startError_) {
            return;
        }

        acceptor_.listen(
            asio::socket_base::max_listen_connections,
            startError_);

        if (startError_) {
            return;
        }

        endpoint_ = acceptor_.local_endpoint(startError_);

        if (startError_) {
            return;
        }

        worker_ = std::jthread { [this] {
            run_();
        } };
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;
    LoopbackServer(LoopbackServer&&) = delete;
    LoopbackServer& operator=(LoopbackServer&&) = delete;

    ~LoopbackServer() {
        if (!worker_.joinable()) {
            return;
        }

        release_.count_down();

        const auto remaining = remainingConnections_.load(
            std::memory_order_acquire);

        // 失败路径唤醒未完成的 accept，避免 fixture 析构挂起。
        for (std::size_t index {}; index < remaining; ++index) {
            asio::io_context wakeContext {};
            asio::ip::tcp::socket wakeSocket { wakeContext };
            std::error_code ignored {};
            wakeSocket.connect(endpoint_, ignored);
        }

        worker_.join();
    }

    [[nodiscard]] bool available() const noexcept {
        return !startError_;
    }

    [[nodiscard]] const std::error_code& start_error() const noexcept {
        return startError_;
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return endpoint_.port();
    }

    [[nodiscard]] bool wait_until_ready() {
        return ready_.try_acquire_for(2s);
    }

    [[nodiscard]] bool accepted() const noexcept {
        return accepted_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool wait_until_accepted() {
        if (accepted()) {
            return true;
        }

        return acceptedSignal_.try_acquire_for(2s);
    }
};

class NonListeningEndpoint final {
private:
    asio::io_context ioContext_ {};
    asio::ip::tcp::acceptor acceptor_ { ioContext_ };
    std::uint16_t port_ {};

public:
    NonListeningEndpoint() {
        std::error_code error {};
        acceptor_.open(asio::ip::tcp::v4(), error);

        if (!error) {
            acceptor_.bind(
                asio::ip::tcp::endpoint {
                    asio::ip::address_v4::loopback(),
                    0
                },
                error);
        }

        if (!error) {
            port_ = acceptor_.local_endpoint(error).port();
        }

        if (error) {
            throw std::system_error { error };
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }
};

enum class ConnectOutcome {
    succeeded,
    cancelled,
    system_error
};

struct ConnectObservation final {
    ConnectOutcome outcome_ {};
    std::thread::id resumedThread_ {};
};

template<typename ReturnScheduler>
Task<std::thread::id> connect_and_observe(
    IoContext& context,
    ReturnScheduler returnTo,
    std::string_view address,
    std::uint16_t port) {
    auto stream = co_await TcpStream::connect(
        context,
        std::move(returnTo),
        address,
        port);
    co_return std::this_thread::get_id();
}

template<typename ReturnScheduler>
Task<ConnectObservation> observe_connect_outcome(
    IoContext& context,
    ReturnScheduler returnTo,
    std::string_view address,
    std::uint16_t port,
    std::stop_token stopToken = {}) {
    try {
        auto stream = co_await TcpStream::connect(
            context,
            std::move(returnTo),
            address,
            port,
            std::move(stopToken));
        co_return ConnectObservation {
            ConnectOutcome::succeeded,
            std::this_thread::get_id()
        };
    } catch (const OperationCancelled&) {
        co_return ConnectObservation {
            ConnectOutcome::cancelled,
            std::this_thread::get_id()
        };
    } catch (const std::system_error&) {
        co_return ConnectObservation {
            ConnectOutcome::system_error,
            std::this_thread::get_id()
        };
    }
}

RunLoop::Scheduler make_expired_scheduler() {
    RunLoop loop {};
    return loop.get_scheduler();
}

template<typename ReturnScheduler>
Task<std::thread::id> write_and_observe(
    TcpStream& stream,
    ReturnScheduler returnTo,
    std::span<const std::byte> buffer) {
    co_await stream.write_all(
        std::move(returnTo),
        buffer);
    co_return std::this_thread::get_id();
}

Task<void> read_into(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::span<std::byte> buffer,
    std::size_t& transferred) {
    transferred = co_await stream.read_some(
        std::move(returnTo),
        buffer);
}

template<typename ReturnScheduler>
Task<void> write_bytes(
    TcpStream& stream,
    ReturnScheduler returnTo,
    std::span<const std::byte> buffer) {
    co_await stream.write_all(
        std::move(returnTo),
        buffer);
}

struct ReadAllObservation final {
    std::vector<std::byte> bytes_ {};
    std::size_t repeatedEof_ {};
    std::thread::id resumedThread_ {};
};

Task<ReadAllObservation> read_until_repeated_eof(
    TcpStream& stream,
    RunLoop::Scheduler returnTo) {
    std::array<std::byte, 2> buffer {};
    std::vector<std::byte> bytes {};

    while (const auto size = co_await stream.read_some(
        returnTo,
        buffer)) {
        bytes.insert(
            bytes.end(),
            buffer.begin(),
            buffer.begin() + static_cast<std::ptrdiff_t>(size));
    }

    const auto repeatedEof = co_await stream.read_some(
        returnTo,
        buffer);
    co_return ReadAllObservation {
        std::move(bytes),
        repeatedEof,
        std::this_thread::get_id()
    };
}

Task<void> run_duplex(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::span<std::byte> readBuffer,
    std::size_t& readSize,
    std::span<const std::byte> writeBuffer) {
    TaskGroup group {};
    group.spawn(read_into(
        stream,
        returnTo,
        readBuffer,
        readSize));
    group.spawn(write_bytes(
        stream,
        returnTo,
        writeBuffer));
    co_await group.join();
}

struct ReadOverlapObservation final {
    bool rejected_ {};
    std::size_t firstRead_ {};
};

Task<ReadOverlapObservation> reject_second_read(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::binary_semaphore& allowServerWrite) {
    std::array<std::byte, 1> firstBuffer {};
    std::array<std::byte, 1> secondBuffer {};
    std::size_t firstRead {};
    bool rejected { false };
    TaskGroup group {};
    group.spawn(read_into(
        stream,
        returnTo,
        firstBuffer,
        firstRead));

    try {
        static_cast<void>(co_await stream.read_some(
            returnTo,
            secondBuffer));
    } catch (const std::logic_error&) {
        rejected = true;
    } catch (...) {
    }

    allowServerWrite.release();
    co_await group.join();
    co_return ReadOverlapObservation { rejected, firstRead };
}

Task<void> occupy_worker(
    ThreadPool::Scheduler scheduler,
    std::binary_semaphore& entered,
    std::atomic<bool>& release) {
    co_await scheduler.schedule();
    entered.release();
    release.wait(false, std::memory_order_acquire);
}

template<typename Resource>
void close_concurrently(Resource& resource) {
    constexpr std::size_t CLOSER_COUNT { 4 };
    std::barrier start { CLOSER_COUNT + 1 };
    std::vector<std::jthread> closers {};
    closers.reserve(CLOSER_COUNT);

    for (std::size_t index {}; index < CLOSER_COUNT; ++index) {
        closers.emplace_back([&resource, &start] {
            start.arrive_and_wait();
            resource.close();
        });
    }

    start.arrive_and_wait();

    for (auto& closer : closers) {
        closer.join();
    }

    resource.close();
}

struct WriteOverlapObservation final {
    bool workerEntered_ {};
    bool rejected_ {};
};

Task<WriteOverlapObservation> reject_second_write(
    TcpStream& stream,
    RunLoop::Scheduler caller,
    ThreadPool::Scheduler gatedReturn) {
    std::binary_semaphore workerEntered { 0 };
    std::atomic<bool> releaseWorker { false };
    TaskGroup group {};
    group.spawn(occupy_worker(
        gatedReturn,
        workerEntered,
        releaseWorker));

    const bool entered = workerEntered.try_acquire_for(2s);

    if (!entered) {
        releaseWorker.store(true, std::memory_order_release);
        releaseWorker.notify_all();
        co_await group.join();
        co_return WriteOverlapObservation {};
    }

    const std::array first {
        std::byte { 0x31 },
        std::byte { 0x32 },
        std::byte { 0x33 },
        std::byte { 0x34 }
    };
    const std::array second { std::byte { 0x35 } };
    group.spawn(write_bytes(
        stream,
        gatedReturn,
        std::span { first }));

    bool rejected { false };

    try {
        co_await stream.write_all(caller, std::span { second });
    } catch (const std::logic_error&) {
        rejected = true;
    } catch (...) {
    }

    releaseWorker.store(true, std::memory_order_release);
    releaseWorker.notify_all();
    co_await group.join();
    co_return WriteOverlapObservation { true, rejected };
}

struct OperationObservation final {
    int completions_ {};
    std::size_t transferred_ {};
    bool cancelled_ {};
    bool logicError_ {};
    bool systemError_ {};
    bool unexpected_ {};
    std::thread::id resumedThread_ {};
};

Task<void> observe_read(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::span<std::byte> buffer,
    OperationObservation& observation,
    std::stop_token stopToken = {}) {
    try {
        observation.transferred_ = co_await stream.read_some(
            std::move(returnTo),
            buffer,
            std::move(stopToken));
    } catch (const OperationCancelled&) {
        observation.cancelled_ = true;
    } catch (const std::logic_error&) {
        observation.logicError_ = true;
    } catch (const std::system_error&) {
        observation.systemError_ = true;
    } catch (...) {
        observation.unexpected_ = true;
    }

    ++observation.completions_;
    observation.resumedThread_ = std::this_thread::get_id();
}

Task<void> observe_write(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::span<const std::byte> buffer,
    OperationObservation& observation,
    std::stop_token stopToken = {}) {
    try {
        co_await stream.write_all(
            std::move(returnTo),
            buffer,
            std::move(stopToken));
    } catch (const OperationCancelled&) {
        observation.cancelled_ = true;
    } catch (const std::logic_error&) {
        observation.logicError_ = true;
    } catch (const std::system_error&) {
        observation.systemError_ = true;
    } catch (...) {
        observation.unexpected_ = true;
    }

    ++observation.completions_;
    observation.resumedThread_ = std::this_thread::get_id();
}

struct ActiveReadCancellationObservation final {
    OperationObservation operation_ {};
    bool overlapRejected_ {};
    std::size_t reuseRead_ {};
};

Task<ActiveReadCancellationObservation> cancel_pending_read(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::stop_source& stopSource,
    std::binary_semaphore& allowServerWrite) {
    std::array<std::byte, 1> pendingBuffer {};
    std::array<std::byte, 1> overlapBuffer {};
    std::array<std::byte, 1> reuseBuffer {};
    ActiveReadCancellationObservation result {};
    TaskGroup group {};
    group.spawn(observe_read(
        stream,
        returnTo,
        pendingBuffer,
        result.operation_,
        stopSource.get_token()));

    try {
        static_cast<void>(co_await stream.read_some(
            returnTo,
            overlapBuffer));
    } catch (const std::logic_error&) {
        result.overlapRejected_ = true;
    } catch (...) {
    }

    stopSource.request_stop();
    co_await group.join();
    allowServerWrite.release();
    result.reuseRead_ = co_await stream.read_some(
        returnTo,
        reuseBuffer);
    co_return result;
}

struct CloseReadObservation final {
    OperationObservation operation_ {};
    bool overlapRejected_ {};
};

Task<CloseReadObservation> close_pending_read(
    TcpStream& stream,
    RunLoop::Scheduler returnTo) {
    std::array<std::byte, 1> pendingBuffer {};
    std::array<std::byte, 1> overlapBuffer {};
    CloseReadObservation result {};
    TaskGroup group {};
    group.spawn(observe_read(
        stream,
        returnTo,
        pendingBuffer,
        result.operation_));

    try {
        static_cast<void>(co_await stream.read_some(
            returnTo,
            overlapBuffer));
    } catch (const std::logic_error&) {
        result.overlapRejected_ = true;
    } catch (...) {
    }

    close_concurrently(stream);
    co_await group.join();
    co_return result;
}

struct ActiveWriteCancellationObservation final {
    OperationObservation operation_ {};
    bool overlapRejected_ {};
};

Task<ActiveWriteCancellationObservation> cancel_pending_write(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::span<const std::byte> buffer,
    std::stop_source& stopSource) {
    const std::array overlapByte { std::byte { 0x11 } };
    ActiveWriteCancellationObservation result {};
    TaskGroup group {};
    group.spawn(observe_write(
        stream,
        returnTo,
        buffer,
        result.operation_,
        stopSource.get_token()));

    try {
        co_await stream.write_all(returnTo, overlapByte);
    } catch (const std::logic_error&) {
        result.overlapRejected_ = true;
    } catch (...) {
    }

    stopSource.request_stop();
    co_await group.join();
    co_return result;
}

Task<OperationObservation> destroy_context_with_pending_read(
    std::unique_ptr<IoContext>& context,
    TcpStream& stream,
    RunLoop::Scheduler returnTo) {
    std::array<std::byte, 1> buffer {};
    OperationObservation result {};
    TaskGroup group {};
    group.spawn(observe_read(
        stream,
        returnTo,
        buffer,
        result));

    std::jthread destroyer { [&context] {
        context.reset();
    } };
    co_await group.join();
    destroyer.join();
    co_return result;
}

Task<OperationObservation> run_close_completion_race(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::barrier<>& start) {
    std::array<std::byte, 1> buffer {};
    OperationObservation result {};
    TaskGroup group {};
    group.spawn(observe_read(
        stream,
        returnTo,
        buffer,
        result));

    std::jthread closer { [&stream, &start] {
        start.arrive_and_wait();
        stream.close();
    } };
    co_await group.join();
    closer.join();
    co_return result;
}

struct RaceCounts final {
    int completed_ {};
    int cancelled_ {};
    int invalid_ {};
};

Task<RaceCounts> run_stop_completion_races(
    TcpStream& stream,
    RunLoop::Scheduler returnTo,
    std::barrier<>& start,
    int raceCount) {
    RaceCounts counts {};

    for (int index { 0 }; index < raceCount; ++index) {
        std::array<std::byte, 1> buffer {};
        std::stop_source stopSource {};
        OperationObservation result {};
        TaskGroup group {};
        group.spawn(observe_read(
            stream,
            returnTo,
            buffer,
            result,
            stopSource.get_token()));

        std::jthread stopper { [&stopSource, &start] {
            start.arrive_and_wait();
            stopSource.request_stop();
        } };
        co_await group.join();
        stopper.join();

        if (result.completions_ != 1 ||
            result.logicError_ ||
            result.systemError_ ||
            result.unexpected_) {
            ++counts.invalid_;
        } else if (result.cancelled_) {
            ++counts.cancelled_;
        } else if (result.transferred_ == 1) {
            ++counts.completed_;
        } else {
            ++counts.invalid_;
        }
    }

    co_return counts;
}

enum class LoadOutcome {
    succeeded,
    cancelled,
    error
};

struct LoadClientObservation final {
    int completions_ {};
    LoadOutcome outcome_ { LoadOutcome::error };
    std::thread::id resumedThread_ {};
};

Task<void> run_echo_client(
    IoContext& context,
    RunLoop::Scheduler returnTo,
    std::uint16_t port,
    std::size_t clientIndex,
    LoadClientObservation& observation) {
    const std::array request {
        std::byte { 0x43 },
        std::byte { 0x4d },
        std::byte { static_cast<unsigned char>(clientIndex) },
        std::byte {
            static_cast<unsigned char>(clientIndex ^ 0x5aU)
        }
    };
    std::array<std::byte, request.size()> reply {};

    try {
        auto stream = co_await TcpStream::connect(
            context,
            returnTo,
            "127.0.0.1",
            port);
        co_await stream.write_all(returnTo, request);

        std::size_t received {};

        while (received < reply.size()) {
            const auto size = co_await stream.read_some(
                returnTo,
                std::span { reply }.subspan(received));

            if (size == 0) {
                break;
            }

            received += size;
        }

        observation.outcome_ =
            received == reply.size() && reply == request
            ? LoadOutcome::succeeded
            : LoadOutcome::error;
    } catch (const OperationCancelled&) {
        observation.outcome_ = LoadOutcome::cancelled;
    } catch (...) {
        observation.outcome_ = LoadOutcome::error;
    }

    ++observation.completions_;
    observation.resumedThread_ = std::this_thread::get_id();
}

struct LoadCounts final {
    int completions_ {};
    int succeeded_ {};
    int cancelled_ {};
    int errors_ {};
    int invalidCompletions_ {};
    int wrongThread_ {};
};

Task<LoadCounts> run_concurrent_echo_clients(
    IoContext& context,
    RunLoop::Scheduler returnTo,
    std::uint16_t port,
    std::size_t clientCount) {
    const auto expectedThread = std::this_thread::get_id();
    std::vector<LoadClientObservation> observations(clientCount);
    TaskGroup group {};

    for (std::size_t index {}; index < clientCount; ++index) {
        group.spawn(run_echo_client(
            context,
            returnTo,
            port,
            index,
            observations[index]));
    }

    co_await group.join();

    LoadCounts counts {};

    for (const auto& observation : observations) {
        counts.completions_ += observation.completions_;
        counts.invalidCompletions_ += observation.completions_ != 1;
        counts.wrongThread_ += observation.resumedThread_ != expectedThread;

        switch (observation.outcome_) {
        case LoadOutcome::succeeded:
            ++counts.succeeded_;
            break;
        case LoadOutcome::cancelled:
            ++counts.cancelled_;
            break;
        case LoadOutcome::error:
            ++counts.errors_;
            break;
        }
    }

    co_return counts;
}

enum class BindOutcome {
    succeeded,
    cancelled,
    system_error
};

struct BindObservation final {
    BindOutcome outcome_ {};
    std::thread::id resumedThread_ {};
};

template<typename ReturnScheduler>
Task<std::thread::id> bind_and_observe(
    IoContext& context,
    ReturnScheduler returnTo,
    std::string_view address,
    std::uint16_t port) {
    auto listener = co_await TcpListener::bind(
        context,
        std::move(returnTo),
        address,
        port);
    co_return std::this_thread::get_id();
}

template<typename ReturnScheduler>
Task<BindObservation> observe_bind_outcome(
    IoContext& context,
    ReturnScheduler returnTo,
    std::string_view address,
    std::uint16_t port,
    std::stop_token stopToken = {}) {
    try {
        auto listener = co_await TcpListener::bind(
            context,
            std::move(returnTo),
            address,
            port,
            std::move(stopToken));
        co_return BindObservation {
            BindOutcome::succeeded,
            std::this_thread::get_id()
        };
    } catch (const OperationCancelled&) {
        co_return BindObservation {
            BindOutcome::cancelled,
            std::this_thread::get_id()
        };
    } catch (const std::system_error&) {
        co_return BindObservation {
            BindOutcome::system_error,
            std::this_thread::get_id()
        };
    }
}

struct AcceptObservation final {
    int completions_ {};
    bool succeeded_ {};
    bool cancelled_ {};
    bool logicError_ {};
    bool systemError_ {};
    bool unexpected_ {};
    std::thread::id resumedThread_ {};
};

template<typename ReturnScheduler>
Task<void> observe_accept(
    TcpListener& listener,
    ReturnScheduler returnTo,
    AcceptObservation& observation,
    std::stop_token stopToken = {}) {
    try {
        auto stream = co_await listener.accept(
            std::move(returnTo),
            std::move(stopToken));
        observation.succeeded_ = true;
    } catch (const OperationCancelled&) {
        observation.cancelled_ = true;
    } catch (const std::logic_error&) {
        observation.logicError_ = true;
    } catch (const std::system_error&) {
        observation.systemError_ = true;
    } catch (...) {
        observation.unexpected_ = true;
    }

    ++observation.completions_;
    observation.resumedThread_ = std::this_thread::get_id();
}

struct ListenerServerObservation final {
    int completions_ {};
    bool succeeded_ {};
    bool cancelled_ {};
    bool error_ {};
    std::thread::id resumedThread_ {};
};

Task<void> accept_and_echo(
    TcpListener& listener,
    RunLoop::Scheduler returnTo,
    bool closeListenerAfterAccept,
    ListenerServerObservation& observation) {
    try {
        auto stream = co_await listener.accept(returnTo);

        if (closeListenerAfterAccept) {
            listener.close();
        }

        std::array<std::byte, 4> payload {};
        std::size_t received {};

        while (received < payload.size()) {
            const auto size = co_await stream.read_some(
                returnTo,
                std::span { payload }.subspan(received));

            if (size == 0) {
                throw std::runtime_error { "listener client closed early" };
            }

            received += size;
        }

        co_await stream.write_all(returnTo, payload);
        observation.succeeded_ = true;
    } catch (const OperationCancelled&) {
        observation.cancelled_ = true;
    } catch (...) {
        observation.error_ = true;
    }

    ++observation.completions_;
    observation.resumedThread_ = std::this_thread::get_id();
}

struct ListenerExchangeObservation final {
    ListenerServerObservation server_ {};
    LoadClientObservation client_ {};
};

Task<ListenerExchangeObservation> run_listener_exchange(
    IoContext& context,
    TcpListener& listener,
    RunLoop::Scheduler returnTo,
    bool closeListenerAfterAccept = false) {
    ListenerExchangeObservation observation {};
    TaskGroup group {};
    group.spawn(accept_and_echo(
        listener,
        returnTo,
        closeListenerAfterAccept,
        observation.server_));
    group.spawn(run_echo_client(
        context,
        returnTo,
        listener.local_port(),
        7,
        observation.client_));
    co_await group.join();
    co_return observation;
}

Task<void> connect_once_to(
    IoContext& context,
    RunLoop::Scheduler returnTo,
    std::string_view numericAddress,
    std::uint16_t port,
    bool& succeeded) {
    try {
        auto stream = co_await TcpStream::connect(
            context,
            std::move(returnTo),
            numericAddress,
            port);
        succeeded = true;
    } catch (...) {
        succeeded = false;
    }
}

struct AcceptConnectionObservation final {
    AcceptObservation accepted_ {};
    bool connected_ {};
};

Task<AcceptConnectionObservation> run_accept_connection(
    IoContext& context,
    TcpListener& listener,
    RunLoop::Scheduler returnTo,
    std::string_view numericAddress) {
    AcceptConnectionObservation result {};
    TaskGroup group {};
    group.spawn(observe_accept(
        listener,
        returnTo,
        result.accepted_));
    group.spawn(connect_once_to(
        context,
        returnTo,
        numericAddress,
        listener.local_port(),
        result.connected_));
    co_await group.join();
    co_return result;
}

template<typename AcceptScheduler>
Task<AcceptConnectionObservation> run_accept_connection_with_return(
    IoContext& context,
    TcpListener& listener,
    AcceptScheduler acceptReturnTo,
    RunLoop::Scheduler clientReturnTo) {
    AcceptConnectionObservation result {};
    TaskGroup group {};
    group.spawn(observe_accept(
        listener,
        std::move(acceptReturnTo),
        result.accepted_));
    group.spawn(connect_once_to(
        context,
        std::move(clientReturnTo),
        "127.0.0.1",
        listener.local_port(),
        result.connected_));
    co_await group.join();
    co_return result;
}

struct SignallingScheduler final {
    ThreadPool::Scheduler scheduler_;
    std::binary_semaphore* entered_ {};

    [[nodiscard]] Task<void> schedule() const {
        entered_->release();
        co_await scheduler_.schedule();
    }
};

template<typename T>
Task<void> store_task_result(Task<T> task, T& result) {
    result = co_await std::move(task);
}

struct BindPublicationObservation final {
    bool workerEntered_ {};
    bool returnEntered_ {};
    BindObservation bind_ {};
};

Task<BindPublicationObservation> destroy_context_before_bind_publication(
    std::unique_ptr<IoContext>& context,
    ThreadPool::Scheduler gatedReturn) {
    BindPublicationObservation result {};
    std::binary_semaphore workerEntered { 0 };
    std::binary_semaphore returnEntered { 0 };
    std::atomic<bool> releaseWorker { false };
    TaskGroup group {};
    group.spawn(occupy_worker(
        gatedReturn,
        workerEntered,
        releaseWorker));
    result.workerEntered_ = workerEntered.try_acquire_for(2s);

    if (result.workerEntered_) {
        group.spawn(store_task_result(
            observe_bind_outcome(
                *context,
                SignallingScheduler { gatedReturn, &returnEntered },
                "127.0.0.1",
                0),
            result.bind_));
        result.returnEntered_ = returnEntered.try_acquire_for(2s);
        context.reset();
    }

    releaseWorker.store(true, std::memory_order_release);
    releaseWorker.notify_all();
    co_await group.join();
    co_return result;
}

struct TcpPublicationObservation final {
    bool workerEntered_ {};
    bool returnEntered_ {};
    ConnectObservation connect_ {};
    AcceptObservation accept_ {};
};

Task<TcpPublicationObservation> destroy_context_before_tcp_publication(
    std::unique_ptr<IoContext>& context,
    TcpListener& listener,
    RunLoop::Scheduler caller,
    ThreadPool::Scheduler gatedReturn) {
    TcpPublicationObservation result {};
    std::binary_semaphore workerEntered { 0 };
    std::binary_semaphore returnEntered { 0 };
    std::atomic<bool> releaseWorker { false };
    TaskGroup group {};
    group.spawn(occupy_worker(
        gatedReturn,
        workerEntered,
        releaseWorker));
    result.workerEntered_ = workerEntered.try_acquire_for(2s);

    if (result.workerEntered_) {
        group.spawn(observe_accept(
            listener,
            SignallingScheduler { gatedReturn, &returnEntered },
            result.accept_));
        group.spawn(store_task_result(
            observe_connect_outcome(
                *context,
                caller,
                "127.0.0.1",
                listener.local_port()),
            result.connect_));
        result.returnEntered_ = returnEntered.try_acquire_for(2s);
        context.reset();
    }

    releaseWorker.store(true, std::memory_order_release);
    releaseWorker.notify_all();
    co_await group.join();
    co_return result;
}

struct AcceptPublicationObservation final {
    bool workerEntered_ {};
    bool returnEntered_ {};
    bool overlapRejected_ {};
    AcceptObservation accepted_ {};
    bool connected_ {};
    bool overlapConnected_ {};
};

Task<AcceptPublicationObservation> reject_accept_before_publication(
    IoContext& context,
    TcpListener& listener,
    RunLoop::Scheduler caller,
    ThreadPool::Scheduler gatedReturn) {
    AcceptPublicationObservation result {};
    std::binary_semaphore workerEntered { 0 };
    std::binary_semaphore returnEntered { 0 };
    std::atomic<bool> releaseWorker { false };
    TaskGroup group {};
    group.spawn(occupy_worker(
        gatedReturn,
        workerEntered,
        releaseWorker));
    result.workerEntered_ = workerEntered.try_acquire_for(2s);

    if (!result.workerEntered_) {
        releaseWorker.store(true, std::memory_order_release);
        releaseWorker.notify_all();
        co_await group.join();
        co_return result;
    }

    group.spawn(observe_accept(
        listener,
        SignallingScheduler { gatedReturn, &returnEntered },
        result.accepted_));
    group.spawn(connect_once_to(
        context,
        caller,
        "127.0.0.1",
        listener.local_port(),
        result.connected_));
    result.returnEntered_ = returnEntered.try_acquire_for(2s);

    if (result.returnEntered_) {
        group.spawn(connect_once_to(
            context,
            caller,
            "127.0.0.1",
            listener.local_port(),
            result.overlapConnected_));

        try {
            auto overlap = co_await listener.accept(caller);
        } catch (const std::logic_error&) {
            result.overlapRejected_ = true;
        } catch (...) {
        }
    } else {
        listener.close();
    }

    releaseWorker.store(true, std::memory_order_release);
    releaseWorker.notify_all();
    co_await group.join();
    co_return result;
}

struct AcceptCancellationObservation final {
    AcceptObservation cancelled_ {};
    AcceptObservation reused_ {};
    bool overlapRejected_ {};
    bool reconnectSucceeded_ {};
};

Task<AcceptCancellationObservation> cancel_pending_accept(
    IoContext& context,
    TcpListener& listener,
    RunLoop::Scheduler returnTo,
    std::stop_source& stopSource) {
    AcceptCancellationObservation result {};
    TaskGroup pending {};
    pending.spawn(observe_accept(
        listener,
        returnTo,
        result.cancelled_,
        stopSource.get_token()));

    try {
        auto overlap = co_await listener.accept(returnTo);
    } catch (const std::logic_error&) {
        result.overlapRejected_ = true;
    } catch (...) {
    }

    stopSource.request_stop();
    co_await pending.join();

    TaskGroup reuse {};
    reuse.spawn(observe_accept(
        listener,
        returnTo,
        result.reused_));
    reuse.spawn(connect_once_to(
        context,
        returnTo,
        "127.0.0.1",
        listener.local_port(),
        result.reconnectSucceeded_));
    co_await reuse.join();
    co_return result;
}

struct CloseAcceptObservation final {
    AcceptObservation operation_ {};
    bool overlapRejected_ {};
};

Task<CloseAcceptObservation> close_pending_accept(
    TcpListener& listener,
    RunLoop::Scheduler returnTo) {
    CloseAcceptObservation result {};
    TaskGroup group {};
    group.spawn(observe_accept(
        listener,
        returnTo,
        result.operation_));

    try {
        auto overlap = co_await listener.accept(returnTo);
    } catch (const std::logic_error&) {
        result.overlapRejected_ = true;
    } catch (...) {
    }

    close_concurrently(listener);
    co_await group.join();
    co_return result;
}

Task<AcceptObservation> destroy_context_with_pending_accept(
    std::unique_ptr<IoContext>& context,
    TcpListener& listener,
    RunLoop::Scheduler returnTo) {
    AcceptObservation result {};
    TaskGroup group {};
    group.spawn(observe_accept(
        listener,
        returnTo,
        result));

    std::jthread destroyer { [&context] {
        context.reset();
    } };
    co_await group.join();
    destroyer.join();
    co_return result;
}

struct AcceptRaceObservation final {
    AcceptObservation operation_ {};
    bool connected_ {};
};

Task<AcceptRaceObservation> run_close_accept_race(
    TcpListener& listener,
    RunLoop::Scheduler returnTo) {
    AcceptRaceObservation result {};
    TaskGroup group {};
    group.spawn(observe_accept(
        listener,
        returnTo,
        result.operation_));

    std::barrier start { 2 };
    const auto port = listener.local_port();
    std::jthread connector { [&start, port, &result] {
        start.arrive_and_wait();
        asio::io_context ioContext {};
        asio::ip::tcp::socket socket { ioContext };
        std::error_code error {};
        socket.connect(
            asio::ip::tcp::endpoint {
                asio::ip::address_v4::loopback(),
                port
            },
            error);
        result.connected_ = !error;
    } };
    std::jthread closer { [&listener, &start] {
        start.arrive_and_wait();
        listener.close();
    } };

    co_await group.join();
    connector.join();
    closer.join();
    co_return result;
}

struct AcceptRaceCounts final {
    int succeeded_ {};
    int cancelled_ {};
    int invalid_ {};
    int connectionErrors_ {};
};

Task<AcceptRaceCounts> run_stop_accept_races(
    TcpListener& listener,
    RunLoop::Scheduler returnTo,
    int raceCount) {
    AcceptRaceCounts counts {};

    for (int iteration { 0 }; iteration < raceCount; ++iteration) {
        std::stop_source stopSource {};
        AcceptObservation operation {};
        TaskGroup group {};
        group.spawn(observe_accept(
            listener,
            returnTo,
            operation,
            stopSource.get_token()));

        std::barrier start { 2 };
        std::atomic<bool> connected { false };
        const auto port = listener.local_port();
        std::jthread connector { [&start, &connected, port] {
            start.arrive_and_wait();
            asio::io_context ioContext {};
            asio::ip::tcp::socket socket { ioContext };
            std::error_code error {};
            socket.connect(
                asio::ip::tcp::endpoint {
                    asio::ip::address_v4::loopback(),
                    port
                },
                error);
            connected.store(!error, std::memory_order_release);
        } };
        std::jthread stopper { [&start, &stopSource] {
            start.arrive_and_wait();
            stopSource.request_stop();
        } };

        co_await group.join();
        connector.join();
        stopper.join();

        counts.connectionErrors_ += !connected.load(
            std::memory_order_acquire);

        if (operation.completions_ != 1 ||
            operation.logicError_ ||
            operation.systemError_ ||
            operation.unexpected_) {
            ++counts.invalid_;
        } else if (operation.succeeded_) {
            ++counts.succeeded_;
        } else if (operation.cancelled_) {
            ++counts.cancelled_;
        } else {
            ++counts.invalid_;
        }
    }

    co_return counts;
}

struct ListenerLoadServerObservation final {
    int completions_ {};
    bool succeeded_ {};
    bool error_ {};
    std::thread::id resumedThread_ {};
};

Task<void> echo_accepted_stream(
    TcpStream stream,
    RunLoop::Scheduler returnTo,
    ListenerLoadServerObservation& observation) {
    try {
        std::array<std::byte, 4> payload {};
        std::size_t received {};

        while (received < payload.size()) {
            const auto size = co_await stream.read_some(
                returnTo,
                std::span { payload }.subspan(received));

            if (size == 0) {
                throw std::runtime_error { "load client closed early" };
            }

            received += size;
        }

        co_await stream.write_all(returnTo, payload);
        observation.succeeded_ = true;
    } catch (...) {
        observation.error_ = true;
    }

    ++observation.completions_;
    observation.resumedThread_ = std::this_thread::get_id();
}

struct ListenerAcceptLoopObservation final {
    int accepted_ {};
    int errors_ {};
    int wrongThread_ {};
};

Task<void> accept_echo_clients(
    TcpListener& listener,
    RunLoop::Scheduler returnTo,
    std::vector<ListenerLoadServerObservation>& observations,
    ListenerAcceptLoopObservation& acceptLoop) {
    const auto expectedThread = std::this_thread::get_id();
    TaskGroup handlers {};

    try {
        for (auto& observation : observations) {
            auto stream = co_await listener.accept(returnTo);
            ++acceptLoop.accepted_;
            acceptLoop.wrongThread_ +=
                std::this_thread::get_id() != expectedThread;
            handlers.spawn(echo_accepted_stream(
                std::move(stream),
                returnTo,
                observation));
        }
    } catch (...) {
        ++acceptLoop.errors_;
        listener.close();
    }

    co_await handlers.join();
}

struct ListenerLoadCounts final {
    LoadCounts clients_ {};
    int accepted_ {};
    int acceptErrors_ {};
    int acceptWrongThread_ {};
    int serverCompletions_ {};
    int serverSuccesses_ {};
    int serverErrors_ {};
    int serverWrongThread_ {};
};

Task<ListenerLoadCounts> run_listener_load(
    IoContext& context,
    TcpListener& listener,
    RunLoop::Scheduler returnTo,
    std::size_t clientCount) {
    const auto expectedThread = std::this_thread::get_id();
    std::vector<ListenerLoadServerObservation> server(clientCount);
    std::vector<LoadClientObservation> clients(clientCount);
    ListenerAcceptLoopObservation acceptLoop {};
    TaskGroup group {};
    group.spawn(accept_echo_clients(
        listener,
        returnTo,
        server,
        acceptLoop));

    for (std::size_t index {}; index < clientCount; ++index) {
        group.spawn(run_echo_client(
            context,
            returnTo,
            listener.local_port(),
            index,
            clients[index]));
    }

    co_await group.join();

    ListenerLoadCounts counts {};
    counts.accepted_ = acceptLoop.accepted_;
    counts.acceptErrors_ = acceptLoop.errors_;
    counts.acceptWrongThread_ = acceptLoop.wrongThread_;

    for (const auto& observation : server) {
        counts.serverCompletions_ += observation.completions_;
        counts.serverSuccesses_ += observation.succeeded_;
        counts.serverErrors_ += observation.error_;
        counts.serverWrongThread_ +=
            observation.resumedThread_ != expectedThread;
    }

    for (const auto& observation : clients) {
        counts.clients_.completions_ += observation.completions_;
        counts.clients_.invalidCompletions_ +=
            observation.completions_ != 1;
        counts.clients_.wrongThread_ +=
            observation.resumedThread_ != expectedThread;

        switch (observation.outcome_) {
        case LoadOutcome::succeeded:
            ++counts.clients_.succeeded_;
            break;
        case LoadOutcome::cancelled:
            ++counts.clients_.cancelled_;
            break;
        case LoadOutcome::error:
            ++counts.clients_.errors_;
            break;
        }
    }

    co_return counts;
}

TEST(CmpTcpTest, ListenerBindIsLazyAndOwnsTemporaryAddress) {
    auto reservation = std::make_unique<NonListeningEndpoint>();
    const auto port = reservation->port();
    IoContext context {};
    RunLoop loop {};
    auto task = TcpListener::bind(
        context,
        loop.get_scheduler(),
        std::string { "127.0.0.1" },
        port);

    reservation.reset();
    auto listener = loop.run(std::move(task));

    EXPECT_TRUE(listener.is_open());
    EXPECT_EQ(listener.local_port(), port);
}

TEST(CmpTcpTest, ListenerBindTaskRejectsExpiredContext) {
    RunLoop loop {};
    std::optional<Task<TcpListener>> bindTask {};

    {
        IoContext context {};
        bindTask.emplace(TcpListener::bind(
            context,
            loop.get_scheduler(),
            "127.0.0.1",
            0));
    }

    EXPECT_THROW(
        loop.run(std::move(*bindTask)),
        std::logic_error);
}

TEST(CmpTcpTest, ListenerAcceptsIpv4AndStreamSurvivesListenerClose) {
    IoContext context {};
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));

    ASSERT_TRUE(listener.is_open());
    ASSERT_NE(listener.local_port(), 0);
    const auto port = listener.local_port();
    const auto observation = loop.run(run_listener_exchange(
        context,
        listener,
        loop.get_scheduler(),
        true));

    EXPECT_FALSE(listener.is_open());
    EXPECT_EQ(listener.local_port(), port);
    EXPECT_EQ(observation.server_.completions_, 1);
    EXPECT_TRUE(observation.server_.succeeded_);
    EXPECT_FALSE(observation.server_.cancelled_);
    EXPECT_FALSE(observation.server_.error_);
    EXPECT_EQ(observation.server_.resumedThread_, callerThread);
    EXPECT_EQ(observation.client_.completions_, 1);
    EXPECT_EQ(observation.client_.outcome_, LoadOutcome::succeeded);
    EXPECT_EQ(observation.client_.resumedThread_, callerThread);
}

TEST(CmpTcpTest, ListenerAcceptsIpv6WhenLoopbackIsAvailable) {
    {
        LoopbackServer capability { asio::ip::address_v6::loopback() };

        if (!capability.available()) {
            GTEST_SKIP() << "IPv6 loopback unavailable: "
                         << capability.start_error().message();
        }
    }

    IoContext context {};
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "::1",
        0));
    const auto observation = loop.run(run_accept_connection(
        context,
        listener,
        loop.get_scheduler(),
        "::1"));

    EXPECT_TRUE(observation.connected_);
    EXPECT_EQ(observation.accepted_.completions_, 1);
    EXPECT_TRUE(observation.accepted_.succeeded_);
    EXPECT_EQ(observation.accepted_.resumedThread_,
              std::this_thread::get_id());
}

TEST(CmpTcpTest, ListenerReportsBindErrorsOnReturnScheduler) {
    NonListeningEndpoint occupied {};
    IoContext context {};
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();

    const auto invalidAddress = loop.run(observe_bind_outcome(
        context,
        loop.get_scheduler(),
        "not-an-address",
        0));
    EXPECT_EQ(invalidAddress.outcome_, BindOutcome::system_error);
    EXPECT_EQ(invalidAddress.resumedThread_, callerThread);

    const auto addressInUse = loop.run(observe_bind_outcome(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        occupied.port()));
    EXPECT_EQ(addressInUse.outcome_, BindOutcome::system_error);
    EXPECT_EQ(addressInUse.resumedThread_, callerThread);
}

TEST(CmpTcpTest, ListenerPreCancellationSkipsInvalidAddressParsing) {
    IoContext context {};
    RunLoop loop {};
    std::stop_source stopSource {};
    stopSource.request_stop();

    const auto observation = loop.run(observe_bind_outcome(
        context,
        loop.get_scheduler(),
        "not-an-address",
        0,
        stopSource.get_token()));

    EXPECT_EQ(observation.outcome_, BindOutcome::cancelled);
    EXPECT_EQ(observation.resumedThread_, std::this_thread::get_id());
}

TEST(CmpTcpTest, ListenerCanReturnThroughThreadPoolScheduler) {
    IoContext context {};
    ThreadPool returnWorkers { 1 };
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    const auto resumedThread = loop.run(bind_and_observe(
        context,
        returnWorkers.get_scheduler(),
        "127.0.0.1",
        0));

    EXPECT_NE(resumedThread, callerThread);
}

TEST(CmpTcpTest, ListenerInvalidReturnSchedulerClosesBoundSocket) {
    auto reservation = std::make_unique<NonListeningEndpoint>();
    const auto port = reservation->port();
    reservation.reset();

    IoContext context {};
    RunLoop driver {};
    RunLoop inactive {};

    EXPECT_THROW(
        driver.run(TcpListener::bind(
            context,
            inactive.get_scheduler(),
            "127.0.0.1",
            port)),
        std::logic_error);

    auto listener = driver.run(TcpListener::bind(
        context,
        driver.get_scheduler(),
        "127.0.0.1",
        port));
    EXPECT_TRUE(listener.is_open());
    EXPECT_EQ(listener.local_port(), port);
}

TEST(CmpTcpTest, ListenerAcceptCanReturnThroughThreadPoolScheduler) {
    IoContext context {};
    ThreadPool returnWorkers { 1 };
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto observation = loop.run(run_accept_connection_with_return(
        context,
        listener,
        returnWorkers.get_scheduler(),
        loop.get_scheduler()));

    EXPECT_TRUE(observation.connected_);
    EXPECT_EQ(observation.accepted_.completions_, 1);
    EXPECT_TRUE(observation.accepted_.succeeded_);
    EXPECT_NE(observation.accepted_.resumedThread_,
              std::this_thread::get_id());
    EXPECT_TRUE(listener.is_open());
}

TEST(CmpTcpTest, ListenerInvalidAcceptReturnSchedulerKeepsItUsable) {
    IoContext context {};
    RunLoop driver {};
    RunLoop inactive {};
    auto listener = driver.run(TcpListener::bind(
        context,
        driver.get_scheduler(),
        "127.0.0.1",
        0));
    const auto observation = driver.run(
        run_accept_connection_with_return(
            context,
            listener,
            inactive.get_scheduler(),
            driver.get_scheduler()));

    EXPECT_TRUE(observation.connected_);
    EXPECT_EQ(observation.accepted_.completions_, 1);
    EXPECT_FALSE(observation.accepted_.succeeded_);
    EXPECT_TRUE(observation.accepted_.logicError_);
    EXPECT_FALSE(observation.accepted_.cancelled_);
    EXPECT_FALSE(observation.accepted_.systemError_);
    EXPECT_FALSE(observation.accepted_.unexpected_);
    EXPECT_TRUE(listener.is_open());

    const auto reuse = driver.run(run_accept_connection(
        context,
        listener,
        driver.get_scheduler(),
        "127.0.0.1"));
    EXPECT_TRUE(reuse.connected_);
    EXPECT_TRUE(reuse.accepted_.succeeded_);
}

TEST(CmpTcpTest, ListenerRejectsOverlapUntilAcceptedStreamIsPublished) {
    IoContext context {};
    ThreadPool gatedReturn { 1 };
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto observation = loop.run(reject_accept_before_publication(
        context,
        listener,
        loop.get_scheduler(),
        gatedReturn.get_scheduler()));

    EXPECT_TRUE(observation.workerEntered_);
    EXPECT_TRUE(observation.returnEntered_);
    EXPECT_TRUE(observation.overlapRejected_);
    EXPECT_TRUE(observation.connected_);
    EXPECT_TRUE(observation.overlapConnected_);
    EXPECT_EQ(observation.accepted_.completions_, 1);
    EXPECT_TRUE(observation.accepted_.succeeded_);
    EXPECT_FALSE(observation.accepted_.cancelled_);
    EXPECT_FALSE(observation.accepted_.logicError_);
    EXPECT_FALSE(observation.accepted_.systemError_);
    EXPECT_FALSE(observation.accepted_.unexpected_);
    EXPECT_NE(observation.accepted_.resumedThread_,
              std::this_thread::get_id());
    EXPECT_TRUE(listener.is_open());
}

TEST(CmpTcpTest, ListenerPreCancelledAcceptLeavesItUsable) {
    IoContext context {};
    RunLoop loop {};
    std::stop_source stopSource {};
    stopSource.request_stop();
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));

    EXPECT_THROW(
        loop.run(listener.accept(
            loop.get_scheduler(),
            stopSource.get_token())),
        OperationCancelled);
    EXPECT_TRUE(listener.is_open());

    const auto reuse = loop.run(run_accept_connection(
        context,
        listener,
        loop.get_scheduler(),
        "127.0.0.1"));
    EXPECT_TRUE(reuse.connected_);
    EXPECT_TRUE(reuse.accepted_.succeeded_);
}

TEST(CmpTcpTest, ListenerCancelsPendingAcceptAndCanAcceptAgain) {
    IoContext context {};
    RunLoop loop {};
    std::stop_source stopSource {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto observation = loop.run(cancel_pending_accept(
        context,
        listener,
        loop.get_scheduler(),
        stopSource));

    EXPECT_TRUE(observation.overlapRejected_);
    EXPECT_EQ(observation.cancelled_.completions_, 1);
    EXPECT_TRUE(observation.cancelled_.cancelled_);
    EXPECT_FALSE(observation.cancelled_.succeeded_);
    EXPECT_FALSE(observation.cancelled_.logicError_);
    EXPECT_FALSE(observation.cancelled_.systemError_);
    EXPECT_FALSE(observation.cancelled_.unexpected_);
    EXPECT_EQ(observation.cancelled_.resumedThread_,
              std::this_thread::get_id());
    EXPECT_TRUE(observation.reconnectSucceeded_);
    EXPECT_EQ(observation.reused_.completions_, 1);
    EXPECT_TRUE(observation.reused_.succeeded_);
    EXPECT_TRUE(listener.is_open());
}

TEST(CmpTcpTest, ListenerCloseCancelsPendingAcceptAndIsIdempotent) {
    IoContext context {};
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto port = listener.local_port();
    const auto observation = loop.run(close_pending_accept(
        listener,
        loop.get_scheduler()));

    EXPECT_TRUE(observation.overlapRejected_);
    EXPECT_EQ(observation.operation_.completions_, 1);
    EXPECT_TRUE(observation.operation_.cancelled_);
    EXPECT_FALSE(observation.operation_.succeeded_);
    EXPECT_FALSE(observation.operation_.logicError_);
    EXPECT_FALSE(observation.operation_.systemError_);
    EXPECT_FALSE(observation.operation_.unexpected_);
    EXPECT_FALSE(listener.is_open());
    EXPECT_EQ(listener.local_port(), port);
    EXPECT_NO_THROW(listener.close());
    EXPECT_THROW(
        loop.run(listener.accept(loop.get_scheduler())),
        std::logic_error);
}

TEST(CmpTcpTest, ListenerMoveTransfersCloseOwnership) {
    IoContext context {};
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto port = listener.local_port();
    TcpListener moved { std::move(listener) };

    EXPECT_FALSE(listener.is_open());
    EXPECT_EQ(listener.local_port(), 0);
    EXPECT_NO_THROW(listener.close());
    EXPECT_THROW(
        loop.run(listener.accept(loop.get_scheduler())),
        std::logic_error);
    EXPECT_TRUE(moved.is_open());
    EXPECT_EQ(moved.local_port(), port);

    moved.close();
    EXPECT_FALSE(moved.is_open());
    EXPECT_EQ(moved.local_port(), port);
}

TEST(CmpTcpTest, ListenerAcceptTaskSurvivesHandleMove) {
    IoContext context {};
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto port = listener.local_port();
    auto acceptTask = listener.accept(loop.get_scheduler());
    TcpListener moved { std::move(listener) };
    bool connected {};
    std::jthread connector { [port, &connected] {
        asio::io_context ioContext {};
        asio::ip::tcp::socket socket { ioContext };
        std::error_code error {};
        socket.connect(
            asio::ip::tcp::endpoint {
                asio::ip::address_v4::loopback(),
                port
            },
            error);
        connected = !error;
    } };

    auto stream = loop.run(std::move(acceptTask));
    connector.join();

    EXPECT_TRUE(connected);
    EXPECT_TRUE(stream.is_open());
    EXPECT_TRUE(moved.is_open());
}

TEST(CmpTcpTest, ListenerContextShutdownDrainsPendingAccept) {
    auto context = std::make_unique<IoContext>();
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        *context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto port = listener.local_port();
    const auto observation = loop.run(
        destroy_context_with_pending_accept(
            context,
            listener,
            loop.get_scheduler()));

    EXPECT_FALSE(context);
    EXPECT_EQ(observation.completions_, 1);
    EXPECT_TRUE(observation.cancelled_);
    EXPECT_FALSE(observation.succeeded_);
    EXPECT_FALSE(observation.logicError_);
    EXPECT_FALSE(observation.systemError_);
    EXPECT_FALSE(observation.unexpected_);
    EXPECT_EQ(observation.resumedThread_, std::this_thread::get_id());
    EXPECT_FALSE(listener.is_open());
    EXPECT_EQ(listener.local_port(), port);
    EXPECT_THROW(
        loop.run(listener.accept(loop.get_scheduler())),
        std::logic_error);
    EXPECT_NO_THROW(listener.close());
}

TEST(CmpTcpTest, ContextShutdownRejectsUnpublishedTcpResources) {
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();

    {
        auto context = std::make_unique<IoContext>();
        ThreadPool gatedReturn { 1 };
        const auto observation = loop.run(
            destroy_context_before_bind_publication(
                context,
                gatedReturn.get_scheduler()));

        EXPECT_FALSE(context);
        EXPECT_TRUE(observation.workerEntered_);
        EXPECT_TRUE(observation.returnEntered_);
        EXPECT_EQ(observation.bind_.outcome_, BindOutcome::cancelled);
        EXPECT_NE(observation.bind_.resumedThread_, callerThread);
    }

    {
        auto context = std::make_unique<IoContext>();
        ThreadPool gatedReturn { 1 };
        auto listener = loop.run(TcpListener::bind(
            *context,
            loop.get_scheduler(),
            "127.0.0.1",
            0));
        const auto observation = loop.run(
            destroy_context_before_tcp_publication(
                context,
                listener,
                loop.get_scheduler(),
                gatedReturn.get_scheduler()));

        EXPECT_FALSE(context);
        EXPECT_TRUE(observation.workerEntered_);
        EXPECT_TRUE(observation.returnEntered_);
        EXPECT_EQ(observation.connect_.outcome_, ConnectOutcome::cancelled);
        EXPECT_EQ(observation.connect_.resumedThread_, callerThread);
        EXPECT_EQ(observation.accept_.completions_, 1);
        EXPECT_TRUE(observation.accept_.cancelled_);
        EXPECT_FALSE(observation.accept_.succeeded_);
        EXPECT_FALSE(observation.accept_.logicError_);
        EXPECT_FALSE(observation.accept_.systemError_);
        EXPECT_FALSE(observation.accept_.unexpected_);
        EXPECT_NE(observation.accept_.resumedThread_, callerThread);
        EXPECT_FALSE(listener.is_open());
    }
}

TEST(CmpTcpTest, ListenerCloseCompletionRaceChoosesOneOutcome) {
    constexpr int RACE_COUNT { 20 };
    IoContext context {};
    RunLoop loop {};
    int succeeded {};
    int cancelled {};
    int invalid {};

    for (int iteration { 0 }; iteration < RACE_COUNT; ++iteration) {
        auto listener = loop.run(TcpListener::bind(
            context,
            loop.get_scheduler(),
            "127.0.0.1",
            0));
        const auto observation = loop.run(run_close_accept_race(
            listener,
            loop.get_scheduler()));

        if (observation.operation_.completions_ != 1 ||
            observation.operation_.logicError_ ||
            observation.operation_.systemError_ ||
            observation.operation_.unexpected_) {
            ++invalid;
        } else if (observation.operation_.succeeded_) {
            ++succeeded;
        } else if (observation.operation_.cancelled_) {
            ++cancelled;
        } else {
            ++invalid;
        }

        EXPECT_FALSE(listener.is_open());
    }

    EXPECT_EQ(succeeded + cancelled, RACE_COUNT);
    EXPECT_EQ(invalid, 0);
}

TEST(CmpTcpTest, ListenerStopCompletionRaceChoosesOneOutcome) {
    constexpr int RACE_COUNT { 100 };
    IoContext context {};
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto counts = loop.run(run_stop_accept_races(
        listener,
        loop.get_scheduler(),
        RACE_COUNT));

    EXPECT_EQ(counts.succeeded_ + counts.cancelled_, RACE_COUNT);
    EXPECT_EQ(counts.invalid_, 0);
    EXPECT_EQ(counts.connectionErrors_, 0);
    EXPECT_TRUE(listener.is_open());
}

TEST(CmpTcpTest, ListenerManyConcurrentClientsCompleteExactlyOnce) {
    constexpr std::size_t CLIENT_COUNT { 32 };
    IoContext context {};
    RunLoop loop {};
    auto listener = loop.run(TcpListener::bind(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        0));
    const auto counts = loop.run(run_listener_load(
        context,
        listener,
        loop.get_scheduler(),
        CLIENT_COUNT));

    EXPECT_EQ(counts.accepted_, static_cast<int>(CLIENT_COUNT));
    EXPECT_EQ(counts.acceptErrors_, 0);
    EXPECT_EQ(counts.acceptWrongThread_, 0);
    EXPECT_EQ(counts.serverCompletions_, static_cast<int>(CLIENT_COUNT));
    EXPECT_EQ(counts.serverSuccesses_, static_cast<int>(CLIENT_COUNT));
    EXPECT_EQ(counts.serverErrors_, 0);
    EXPECT_EQ(counts.serverWrongThread_, 0);
    EXPECT_EQ(counts.clients_.completions_,
              static_cast<int>(CLIENT_COUNT));
    EXPECT_EQ(counts.clients_.succeeded_,
              static_cast<int>(CLIENT_COUNT));
    EXPECT_EQ(counts.clients_.cancelled_, 0);
    EXPECT_EQ(counts.clients_.errors_, 0);
    EXPECT_EQ(counts.clients_.invalidCompletions_, 0);
    EXPECT_EQ(counts.clients_.wrongThread_, 0);
    EXPECT_TRUE(listener.is_open());
}

TEST(CmpTcpTest, IoContextStartsAndStopsCleanly) {
    IoContext context {};
}

TEST(CmpTcpTest, ConnectIsLazyAndOwnsTemporaryAddress) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto task = TcpStream::connect(
        context,
        loop.get_scheduler(),
        std::string { "127.0.0.1" },
        server.port());

    EXPECT_FALSE(server.accepted());
    auto stream = loop.run(std::move(task));
    EXPECT_TRUE(server.wait_until_accepted());
}

TEST(CmpTcpTest, ConnectsIpv4AndReturnsToRunLoop) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    const auto resumedThread = loop.run(connect_and_observe(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));

    EXPECT_EQ(resumedThread, callerThread);
    EXPECT_TRUE(server.wait_until_accepted());
}

TEST(CmpTcpTest, ConnectsIpv6WhenLoopbackIsAvailable) {
    LoopbackServer server { asio::ip::address_v6::loopback() };

    if (!server.available()) {
        GTEST_SKIP() << "IPv6 loopback unavailable: "
                     << server.start_error().message();
    }

    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    const auto resumedThread = loop.run(connect_and_observe(
        context,
        loop.get_scheduler(),
        "::1",
        server.port()));

    EXPECT_EQ(resumedThread, std::this_thread::get_id());
    EXPECT_TRUE(server.wait_until_accepted());
}

TEST(CmpTcpTest, ReportsAddressAndConnectErrorsOnReturnScheduler) {
    NonListeningEndpoint endpoint {};
    IoContext context {};
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();

    const auto invalidAddress = loop.run(observe_connect_outcome(
        context,
        loop.get_scheduler(),
        "not-an-address",
        endpoint.port()));
    EXPECT_EQ(invalidAddress.outcome_, ConnectOutcome::system_error);
    EXPECT_EQ(invalidAddress.resumedThread_, callerThread);

    const auto refused = loop.run(observe_connect_outcome(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        endpoint.port()));
    EXPECT_EQ(refused.outcome_, ConnectOutcome::system_error);
    EXPECT_EQ(refused.resumedThread_, callerThread);
}

TEST(CmpTcpTest, PreCancellationSkipsInvalidAddressParsing) {
    IoContext context {};
    RunLoop loop {};
    std::stop_source stopSource {};
    stopSource.request_stop();

    const auto observation = loop.run(observe_connect_outcome(
        context,
        loop.get_scheduler(),
        "not-an-address",
        0,
        stopSource.get_token()));

    EXPECT_EQ(observation.outcome_, ConnectOutcome::cancelled);
    EXPECT_EQ(observation.resumedThread_, std::this_thread::get_id());
}

TEST(CmpTcpTest, CanReturnThroughThreadPoolScheduler) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    ThreadPool returnWorkers { 1 };
    RunLoop loop {};
    const auto callerThread = std::this_thread::get_id();
    const auto resumedThread = loop.run(connect_and_observe(
        context,
        returnWorkers.get_scheduler(),
        "127.0.0.1",
        server.port()));

    EXPECT_NE(resumedThread, callerThread);
    EXPECT_TRUE(server.wait_until_accepted());
}

TEST(CmpTcpTest, RejectsInactiveAndExpiredReturnSchedulers) {
    {
        LoopbackServer server { asio::ip::address_v4::loopback() };
        ASSERT_TRUE(server.available()) << server.start_error().message();
        ASSERT_TRUE(server.wait_until_ready());

        IoContext context {};
        RunLoop driver {};
        RunLoop inactive {};

        EXPECT_THROW(
            driver.run(TcpStream::connect(
                context,
                inactive.get_scheduler(),
                "127.0.0.1",
                server.port())),
            std::logic_error);
        EXPECT_TRUE(server.wait_until_accepted());
    }

    {
        LoopbackServer server { asio::ip::address_v4::loopback() };
        ASSERT_TRUE(server.available()) << server.start_error().message();
        ASSERT_TRUE(server.wait_until_ready());

        IoContext context {};
        RunLoop driver {};

        EXPECT_THROW(
            driver.run(TcpStream::connect(
                context,
                make_expired_scheduler(),
                "127.0.0.1",
                server.port())),
            std::logic_error);
        EXPECT_TRUE(server.wait_until_accepted());
    }
}

TEST(CmpTcpTest, EmptyReadAndWriteDoNotTouchTheSocket) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));

    EXPECT_NO_THROW(loop.run(stream.write_all(
        loop.get_scheduler(),
        std::span<const std::byte> {})));
    EXPECT_EQ(
        loop.run(stream.read_some(
            loop.get_scheduler(),
            std::span<std::byte> {})),
        0U);
}

TEST(CmpTcpTest, WritesTheEntireBorrowedBuffer) {
    const std::array expected {
        std::byte { 0x41 },
        std::byte { 0x42 },
        std::byte { 0x43 },
        std::byte { 0x44 }
    };
    std::array<std::byte, expected.size()> received {};
    std::binary_semaphore serverDone { 0 };
    std::atomic<bool> serverSucceeded { false };
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            std::error_code error {};
            const auto size = asio::read(
                socket,
                asio::buffer(received),
                error);
            serverSucceeded.store(
                !error && size == received.size(),
                std::memory_order_release);
            serverDone.release();
        }
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto resumedThread = loop.run(write_and_observe(
        stream,
        loop.get_scheduler(),
        std::span { expected }));

    ASSERT_TRUE(serverDone.try_acquire_for(2s));
    EXPECT_TRUE(serverSucceeded.load(std::memory_order_acquire));
    EXPECT_EQ(received, expected);
    EXPECT_EQ(resumedThread, std::this_thread::get_id());
}

TEST(CmpTcpTest, ReadSomeReturnsAvailablePartialData) {
    const std::array first {
        std::byte { 0x51 },
        std::byte { 0x52 }
    };
    const std::array second {
        std::byte { 0x53 },
        std::byte { 0x54 }
    };
    std::binary_semaphore firstSent { 0 };
    std::binary_semaphore allowSecond { 0 };
    std::binary_semaphore serverDone { 0 };
    std::atomic<bool> serverSucceeded { false };
    std::atomic<bool> gateTimedOut { false };
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            std::error_code error {};
            const auto firstSize = asio::write(
                socket,
                asio::buffer(first),
                error);
            firstSent.release();

            if (!allowSecond.try_acquire_for(2s)) {
                gateTimedOut.store(true, std::memory_order_release);
            }

            const auto secondSize = error
                ? 0
                : asio::write(
                    socket,
                    asio::buffer(second),
                    error);
            serverSucceeded.store(
                !error &&
                    firstSize == first.size() &&
                    secondSize == second.size(),
                std::memory_order_release);
            serverDone.release();
        }
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    ASSERT_TRUE(firstSent.try_acquire_for(2s));

    std::array<std::byte, 8> buffer {};
    const auto firstRead = loop.run(stream.read_some(
        loop.get_scheduler(),
        buffer));
    EXPECT_EQ(firstRead, first.size());
    EXPECT_TRUE(std::ranges::equal(
        std::span { buffer }.first(firstRead),
        first));

    allowSecond.release();
    const auto secondRead = loop.run(stream.read_some(
        loop.get_scheduler(),
        buffer));
    EXPECT_EQ(secondRead, second.size());
    EXPECT_TRUE(std::ranges::equal(
        std::span { buffer }.first(secondRead),
        second));

    ASSERT_TRUE(serverDone.try_acquire_for(2s));
    EXPECT_FALSE(gateTimedOut.load(std::memory_order_acquire));
    EXPECT_TRUE(serverSucceeded.load(std::memory_order_acquire));
}

TEST(CmpTcpTest, DeliversAllBytesBeforeStickyEof) {
    const std::vector payload {
        std::byte { 0x61 },
        std::byte { 0x62 },
        std::byte { 0x63 },
        std::byte { 0x64 },
        std::byte { 0x65 }
    };
    std::binary_semaphore serverDone { 0 };
    std::atomic<bool> serverSucceeded { false };
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            std::error_code error {};
            const auto size = asio::write(
                socket,
                asio::buffer(payload),
                error);
            serverSucceeded.store(
                !error && size == payload.size(),
                std::memory_order_release);
            serverDone.release();
        }
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto observation = loop.run(read_until_repeated_eof(
        stream,
        loop.get_scheduler()));

    ASSERT_TRUE(serverDone.try_acquire_for(2s));
    EXPECT_TRUE(serverSucceeded.load(std::memory_order_acquire));
    EXPECT_EQ(observation.bytes_, payload);
    EXPECT_EQ(observation.repeatedEof_, 0U);
    EXPECT_EQ(observation.resumedThread_, std::this_thread::get_id());
    EXPECT_TRUE(stream.is_open());
}

TEST(CmpTcpTest, SupportsOneSimultaneousReadAndWrite) {
    const std::array request {
        std::byte { 0x71 },
        std::byte { 0x72 },
        std::byte { 0x73 }
    };
    const std::array response {
        std::byte { 0x74 },
        std::byte { 0x75 }
    };
    std::array<std::byte, request.size()> received {};
    std::binary_semaphore serverDone { 0 };
    std::atomic<bool> serverSucceeded { false };
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            std::error_code error {};
            const auto written = asio::write(
                socket,
                asio::buffer(response),
                error);
            const auto read = error
                ? 0
                : asio::read(
                    socket,
                    asio::buffer(received),
                    error);
            serverSucceeded.store(
                !error &&
                    written == response.size() &&
                    read == request.size(),
                std::memory_order_release);
            serverDone.release();
        }
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    std::array<std::byte, response.size()> readBuffer {};
    std::size_t readSize {};
    loop.run(run_duplex(
        stream,
        loop.get_scheduler(),
        readBuffer,
        readSize,
        request));

    ASSERT_TRUE(serverDone.try_acquire_for(2s));
    EXPECT_TRUE(serverSucceeded.load(std::memory_order_acquire));
    EXPECT_EQ(readSize, response.size());
    EXPECT_EQ(readBuffer, response);
    EXPECT_EQ(received, request);
}

TEST(CmpTcpTest, RejectsASecondPendingRead) {
    std::binary_semaphore allowServerWrite { 0 };
    std::binary_semaphore serverDone { 0 };
    std::atomic<bool> gateTimedOut { false };
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            const bool released = allowServerWrite.try_acquire_for(2s);
            gateTimedOut.store(!released, std::memory_order_release);
            const std::array oneByte { std::byte { 0x7a } };
            const std::array twoBytes {
                std::byte { 0x7a },
                std::byte { 0x7b }
            };
            std::error_code ignored {};

            if (released) {
                asio::write(socket, asio::buffer(oneByte), ignored);
            } else {
                asio::write(socket, asio::buffer(twoBytes), ignored);
            }

            serverDone.release();
        }
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto observation = loop.run(reject_second_read(
        stream,
        loop.get_scheduler(),
        allowServerWrite));

    ASSERT_TRUE(serverDone.try_acquire_for(2s));
    EXPECT_FALSE(gateTimedOut.load(std::memory_order_acquire));
    EXPECT_TRUE(observation.rejected_);
    EXPECT_EQ(observation.firstRead_, 1U);
}

TEST(CmpTcpTest, RejectsASecondPendingWrite) {
    const std::array expected {
        std::byte { 0x31 },
        std::byte { 0x32 },
        std::byte { 0x33 },
        std::byte { 0x34 }
    };
    std::array<std::byte, expected.size()> received {};
    std::binary_semaphore serverDone { 0 };
    std::atomic<bool> serverSucceeded { false };
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            std::error_code error {};
            const auto size = asio::read(
                socket,
                asio::buffer(received),
                error);
            serverSucceeded.store(
                !error && size == received.size(),
                std::memory_order_release);
            serverDone.release();
        }
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    ThreadPool gatedReturn { 1 };
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto observation = loop.run(reject_second_write(
        stream,
        loop.get_scheduler(),
        gatedReturn.get_scheduler()));

    ASSERT_TRUE(serverDone.try_acquire_for(2s));
    EXPECT_TRUE(serverSucceeded.load(std::memory_order_acquire));
    EXPECT_TRUE(observation.workerEntered_);
    EXPECT_TRUE(observation.rejected_);
    EXPECT_EQ(received, expected);
}

TEST(CmpTcpTest, CancelsPendingReadAndKeepsStreamUsable) {
    std::binary_semaphore allowServerWrite { 0 };
    std::binary_semaphore serverDone { 0 };
    std::atomic<bool> gateTimedOut { false };
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            const bool released = allowServerWrite.try_acquire_for(2s);
            gateTimedOut.store(!released, std::memory_order_release);
            const std::array oneByte { std::byte { 0x21 } };
            const std::array twoBytes {
                std::byte { 0x21 },
                std::byte { 0x22 }
            };
            std::error_code ignored {};

            if (released) {
                asio::write(socket, asio::buffer(oneByte), ignored);
            } else {
                asio::write(socket, asio::buffer(twoBytes), ignored);
            }

            serverDone.release();
        }
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    std::stop_source stopSource {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto observation = loop.run(cancel_pending_read(
        stream,
        loop.get_scheduler(),
        stopSource,
        allowServerWrite));

    ASSERT_TRUE(serverDone.try_acquire_for(2s));
    EXPECT_FALSE(gateTimedOut.load(std::memory_order_acquire));
    EXPECT_TRUE(observation.overlapRejected_);
    EXPECT_EQ(observation.operation_.completions_, 1);
    EXPECT_TRUE(observation.operation_.cancelled_);
    EXPECT_FALSE(observation.operation_.logicError_);
    EXPECT_FALSE(observation.operation_.systemError_);
    EXPECT_FALSE(observation.operation_.unexpected_);
    EXPECT_EQ(observation.operation_.resumedThread_,
              std::this_thread::get_id());
    EXPECT_EQ(observation.reuseRead_, 1U);
    EXPECT_TRUE(stream.is_open());
}

TEST(CmpTcpTest, PreCancellationLeavesOpenStreamUsable) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    std::stop_source stopSource {};
    stopSource.request_stop();
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    std::array<std::byte, 1> buffer {};

    EXPECT_THROW(
        loop.run(stream.read_some(
            loop.get_scheduler(),
            buffer,
            stopSource.get_token())),
        OperationCancelled);
    EXPECT_THROW(
        loop.run(stream.write_all(
            loop.get_scheduler(),
            buffer,
            stopSource.get_token())),
        OperationCancelled);
    EXPECT_TRUE(stream.is_open());
}

TEST(CmpTcpTest, CloseCancelsPendingReadAndIsIdempotent) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto observation = loop.run(close_pending_read(
        stream,
        loop.get_scheduler()));

    EXPECT_TRUE(observation.overlapRejected_);
    EXPECT_EQ(observation.operation_.completions_, 1);
    EXPECT_TRUE(observation.operation_.cancelled_);
    EXPECT_FALSE(observation.operation_.logicError_);
    EXPECT_FALSE(observation.operation_.systemError_);
    EXPECT_FALSE(observation.operation_.unexpected_);
    EXPECT_FALSE(stream.is_open());
    EXPECT_NO_THROW(stream.close());

    std::stop_source stopSource {};
    stopSource.request_stop();
    std::array<std::byte, 1> buffer {};
    EXPECT_THROW(
        loop.run(stream.read_some(
            loop.get_scheduler(),
            buffer,
            stopSource.get_token())),
        std::logic_error);
    EXPECT_THROW(
        loop.run(stream.write_all(
            loop.get_scheduler(),
            buffer,
            stopSource.get_token())),
        std::logic_error);
}

TEST(CmpTcpTest, MoveTransfersCloseOwnershipAndMovedFromUseFails) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    TcpStream moved { std::move(stream) };
    std::array<std::byte, 1> buffer {};

    EXPECT_FALSE(stream.is_open());
    EXPECT_TRUE(moved.is_open());
    EXPECT_NO_THROW(stream.close());
    EXPECT_THROW(
        loop.run(stream.read_some(loop.get_scheduler(), buffer)),
        std::logic_error);
    EXPECT_THROW(
        loop.run(stream.write_all(loop.get_scheduler(), buffer)),
        std::logic_error);

    moved.close();
    moved.close();
    EXPECT_FALSE(moved.is_open());
}

TEST(CmpTcpTest, CancellingPendingWriteClosesStream) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    std::stop_source stopSource {};
    std::vector<std::byte> buffer(
        8U * 1024U * 1024U,
        std::byte { 0x2a });
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto observation = loop.run(cancel_pending_write(
        stream,
        loop.get_scheduler(),
        buffer,
        stopSource));

    EXPECT_TRUE(observation.overlapRejected_);
    EXPECT_EQ(observation.operation_.completions_, 1);
    EXPECT_TRUE(observation.operation_.cancelled_);
    EXPECT_FALSE(observation.operation_.logicError_);
    EXPECT_FALSE(observation.operation_.systemError_);
    EXPECT_FALSE(observation.operation_.unexpected_);
    EXPECT_FALSE(stream.is_open());
}

TEST(CmpTcpTest, CloseCompletionRaceChoosesOneOutcome) {
    constexpr int RACE_COUNT { 20 };
    IoContext context {};
    RunLoop loop {};
    int completed {};
    int cancelled {};
    int invalid {};

    for (int iteration { 0 }; iteration < RACE_COUNT; ++iteration) {
        std::barrier start { 2 };
        LoopbackServer server {
            asio::ip::address_v4::loopback(),
            [&](asio::ip::tcp::socket& socket) {
                start.arrive_and_wait();
                const std::array byte { std::byte { 0x2b } };
                std::error_code ignored {};
                asio::write(socket, asio::buffer(byte), ignored);
            }
        };
        ASSERT_TRUE(server.available()) << server.start_error().message();
        ASSERT_TRUE(server.wait_until_ready());

        auto stream = loop.run(TcpStream::connect(
            context,
            loop.get_scheduler(),
            "127.0.0.1",
            server.port()));
        const auto observation = loop.run(run_close_completion_race(
            stream,
            loop.get_scheduler(),
            start));

        if (observation.completions_ != 1 ||
            observation.logicError_ ||
            observation.systemError_ ||
            observation.unexpected_) {
            ++invalid;
        } else if (observation.cancelled_) {
            ++cancelled;
        } else if (observation.transferred_ == 1) {
            ++completed;
        } else {
            ++invalid;
        }

        EXPECT_FALSE(stream.is_open());
    }

    EXPECT_EQ(completed + cancelled, RACE_COUNT);
    EXPECT_EQ(invalid, 0);
}

TEST(CmpTcpTest, StopCompletionRaceChoosesOneOutcome) {
    constexpr int RACE_COUNT { 100 };
    std::barrier start { 2 };
    std::binary_semaphore serverDone { 0 };
    std::atomic<bool> serverSucceeded { true };
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            for (int iteration { 0 }; iteration < RACE_COUNT; ++iteration) {
                start.arrive_and_wait();

                if (serverSucceeded.load(std::memory_order_acquire)) {
                    const std::array byte { std::byte { 0x2c } };
                    std::error_code error {};
                    asio::write(socket, asio::buffer(byte), error);

                    if (error) {
                        serverSucceeded.store(false, std::memory_order_release);
                    }
                }
            }

            serverDone.release();
        }
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto counts = loop.run(run_stop_completion_races(
        stream,
        loop.get_scheduler(),
        start,
        RACE_COUNT));

    ASSERT_TRUE(serverDone.try_acquire_for(2s));
    EXPECT_TRUE(serverSucceeded.load(std::memory_order_acquire));
    EXPECT_EQ(counts.completed_ + counts.cancelled_, RACE_COUNT);
    EXPECT_EQ(counts.invalid_, 0);
    EXPECT_TRUE(stream.is_open());
}

TEST(CmpTcpTest, ContextShutdownDrainsPendingReadAndClosesSurvivor) {
    LoopbackServer server { asio::ip::address_v4::loopback() };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    auto context = std::make_unique<IoContext>();
    RunLoop loop {};
    auto stream = loop.run(TcpStream::connect(
        *context,
        loop.get_scheduler(),
        "127.0.0.1",
        server.port()));
    const auto observation = loop.run(destroy_context_with_pending_read(
        context,
        stream,
        loop.get_scheduler()));
    std::array<std::byte, 1> buffer {};

    EXPECT_FALSE(context);
    EXPECT_EQ(observation.completions_, 1);
    EXPECT_TRUE(observation.cancelled_);
    EXPECT_FALSE(observation.logicError_);
    EXPECT_FALSE(observation.systemError_);
    EXPECT_FALSE(observation.unexpected_);
    EXPECT_EQ(observation.resumedThread_, std::this_thread::get_id());
    EXPECT_FALSE(stream.is_open());
    EXPECT_THROW(
        loop.run(stream.read_some(loop.get_scheduler(), buffer)),
        std::logic_error);
    EXPECT_THROW(
        loop.run(stream.write_all(loop.get_scheduler(), buffer)),
        std::logic_error);
    EXPECT_NO_THROW(stream.close());
}

TEST(CmpTcpTest, ManyConcurrentClientsCompleteExactlyOnce) {
    constexpr std::size_t CLIENT_COUNT { 32 };
    constexpr std::size_t PAYLOAD_SIZE { 4 };
    std::atomic<int> serverCompletions {};
    std::atomic<int> serverErrors {};
    LoopbackServer server {
        asio::ip::address_v4::loopback(),
        [&](asio::ip::tcp::socket& socket) {
            std::array<std::byte, PAYLOAD_SIZE> buffer {};
            std::error_code error {};
            const auto received = asio::read(
                socket,
                asio::buffer(buffer),
                error);
            std::size_t written {};

            if (!error && received == buffer.size()) {
                written = asio::write(
                    socket,
                    asio::buffer(buffer),
                    error);
            }

            if (error ||
                received != buffer.size() ||
                written != buffer.size()) {
                serverErrors.fetch_add(1, std::memory_order_relaxed);
            }

            serverCompletions.fetch_add(1, std::memory_order_release);
        },
        CLIENT_COUNT
    };
    ASSERT_TRUE(server.available()) << server.start_error().message();
    ASSERT_TRUE(server.wait_until_ready());

    IoContext context {};
    RunLoop loop {};
    const auto counts = loop.run(run_concurrent_echo_clients(
        context,
        loop.get_scheduler(),
        server.port(),
        CLIENT_COUNT));

    EXPECT_EQ(counts.completions_, static_cast<int>(CLIENT_COUNT));
    EXPECT_EQ(counts.succeeded_, static_cast<int>(CLIENT_COUNT));
    EXPECT_EQ(counts.cancelled_, 0);
    EXPECT_EQ(counts.errors_, 0);
    EXPECT_EQ(counts.invalidCompletions_, 0);
    EXPECT_EQ(counts.wrongThread_, 0);
    EXPECT_EQ(
        serverCompletions.load(std::memory_order_acquire),
        static_cast<int>(CLIENT_COUNT));
    EXPECT_EQ(serverErrors.load(std::memory_order_relaxed), 0);
}

}  // namespace
