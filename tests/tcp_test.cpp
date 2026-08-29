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
    Protocol protocol_ {};
    std::jthread worker_ {};

    void run_() noexcept {
        ready_.release();

        asio::ip::tcp::socket socket { ioContext_ };
        std::error_code error {};
        acceptor_.accept(socket, error);

        if (error) {
            return;
        }

        accepted_.store(true, std::memory_order_release);
        acceptedSignal_.release();

        if (protocol_) {
            std::invoke(protocol_, socket);
        } else {
            release_.wait();
        }
    }

public:
    explicit LoopbackServer(
        const asio::ip::address& address,
        Protocol protocol = {})
        : protocol_ { std::move(protocol) } {
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

        if (!accepted_.load(std::memory_order_acquire)) {
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

}  // namespace
