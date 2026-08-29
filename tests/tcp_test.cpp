#include <gtest/gtest.h>

import std;
import asio;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::IoContext;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
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
private:
    asio::io_context ioContext_ {};
    asio::ip::tcp::acceptor acceptor_ { ioContext_ };
    asio::ip::tcp::endpoint endpoint_ {};
    std::error_code startError_ {};
    std::binary_semaphore ready_ { 0 };
    std::binary_semaphore acceptedSignal_ { 0 };
    std::latch release_ { 1 };
    std::atomic<bool> accepted_ { false };
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
        release_.wait();
    }

public:
    explicit LoopbackServer(const asio::ip::address& address) {
        const asio::ip::tcp::endpoint requested { address, 0 };
        acceptor_.open(requested.protocol(), startError_);

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

}  // namespace
