#if defined(_WIN32)
#error "The loopback benchmark currently requires POSIX sockets"
#endif

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::IoContext;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::TcpStream;
using mcpplibs::cmp::ThreadPool;
using mcpplibs::cmp::run_blocking;

using Scheduler = RunLoop::Scheduler;
using BlockingScheduler = ThreadPool::Scheduler;
using Clock = std::chrono::steady_clock;

constexpr int COMPUTE_TASKS { 50'000 };
constexpr int COMPUTE_STEPS { 128 };
constexpr int FILE_WORKERS { 4 };
constexpr int FILE_ROUNDS { 250 };
constexpr int FILE_FAILURES { 25 };
constexpr int NETWORK_WORKERS { 4 };
constexpr int NETWORK_ROUNDS { 5'000 };
constexpr int NETWORK_FAILURES { 25 };
constexpr std::size_t FILE_PAYLOAD_SIZE { 64 * 1024 };
constexpr std::size_t NETWORK_PAYLOAD_SIZE { 256 };

struct Counters final {
    std::atomic<std::size_t> successes_ {};
    std::atomic<std::size_t> expectedFailures_ {};
    std::atomic<std::size_t> unexpectedFailures_ {};
};

struct Metrics final {
    std::string scenario_ {};
    std::size_t operations_ {};
    std::size_t successes_ {};
    std::size_t expectedFailures_ {};
    std::size_t unexpectedFailures_ {};
    double elapsedMilliseconds_ {};

    [[nodiscard]] bool passed() const noexcept {
        return unexpectedFailures_ == 0 &&
            successes_ + expectedFailures_ == operations_;
    }
};

class Socket final {
private:
    int handle_ { -1 };

public:
    Socket() noexcept = default;

    explicit Socket(int handle) noexcept
        : handle_ { handle } {}

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept
        : handle_ { std::exchange(other.handle_, -1) } {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, -1);
        }
        return *this;
    }

    ~Socket() {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return handle_;
    }

    void reset() noexcept {
        if (handle_ >= 0) {
            ::close(handle_);
            handle_ = -1;
        }
    }
};

class TemporaryDirectory final {
private:
    std::filesystem::path path_ {};

public:
    TemporaryDirectory() {
        const auto suffix = Clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            std::format("cmp-v1-readiness-{}", suffix);

        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error { "cannot create benchmark directory" };
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    ~TemporaryDirectory() {
        std::error_code error {};
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
};

[[noreturn]] void throw_socket_error(std::string_view operation) {
    throw std::system_error {
        errno,
        std::generic_category(),
        std::string { operation }
    };
}

[[nodiscard]] Socket make_socket() {
    const int handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0) {
        throw_socket_error("socket");
    }
    return Socket { handle };
}

[[nodiscard]] std::pair<Socket, std::uint16_t> bind_loopback_socket() {
    auto socket = make_socket();
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        throw_socket_error("bind");
    }

    socklen_t addressLength { sizeof(address) };
    if (::getsockname(
            socket.get(),
            reinterpret_cast<sockaddr*>(&address),
            &addressLength) != 0) {
        throw_socket_error("getsockname");
    }

    return { std::move(socket), ntohs(address.sin_port) };
}

void send_all(int socket, std::span<const std::byte> data) {
    std::size_t offset { 0 };

    while (offset < data.size()) {
        const auto sent = ::send(
            socket,
            data.data() + offset,
            data.size() - offset,
            0);
        if (sent <= 0) {
            throw_socket_error("send");
        }
        offset += static_cast<std::size_t>(sent);
    }
}

void receive_exactly(int socket, std::span<std::byte> data) {
    std::size_t offset { 0 };

    while (offset < data.size()) {
        const auto received = ::recv(
            socket,
            data.data() + offset,
            data.size() - offset,
            0);
        if (received <= 0) {
            throw_socket_error("recv");
        }
        offset += static_cast<std::size_t>(received);
    }
}

void run_echo_server(
    int listener,
    const std::array<std::byte, NETWORK_PAYLOAD_SIZE>& payload) {
    const int acceptedHandle = ::accept(listener, nullptr, nullptr);

    if (acceptedHandle < 0) {
        throw_socket_error("accept");
    }

    Socket accepted { acceptedHandle };
    std::array<std::byte, NETWORK_PAYLOAD_SIZE> request {};

    for (int round { 0 }; round < NETWORK_ROUNDS; ++round) {
        receive_exactly(accepted.get(), request);

        if (!std::ranges::equal(request, payload)) {
            throw std::runtime_error { "server payload mismatch" };
        }

        send_all(accepted.get(), payload);
    }
}

Task<void> failing_compute(Scheduler scheduler) {
    co_await scheduler.schedule();
    throw std::invalid_argument { "expected compute failure" };
}

Task<void> compute_operation(
    Scheduler scheduler,
    int index,
    Counters& counters,
    std::atomic<std::uint64_t>& checksum) {
    if (index % 50 == 0) {
        try {
            co_await failing_compute(scheduler);
        } catch (const std::invalid_argument&) {
            counters.expectedFailures_.fetch_add(1);
        } catch (...) {
            counters.unexpectedFailures_.fetch_add(1);
        }
        co_return;
    }

    co_await scheduler.schedule();
    std::uint64_t value = static_cast<std::uint64_t>(index) + 1;
    for (int step { 0 }; step < COMPUTE_STEPS; ++step) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }

    checksum.fetch_xor(value, std::memory_order_relaxed);
    counters.successes_.fetch_add(1);
}

Task<void> run_compute(Scheduler scheduler, Counters& counters) {
    std::atomic<std::uint64_t> checksum {};
    TaskGroup group {};

    for (int index { 0 }; index < COMPUTE_TASKS; ++index) {
        group.spawn(compute_operation(
            scheduler,
            index,
            counters,
            checksum));
    }

    co_await group.join();
    if (checksum.load(std::memory_order_relaxed) == 0) {
        counters.unexpectedFailures_.fetch_add(1);
    }
}

Task<void> run_file_io(
    BlockingScheduler blockingWorkers,
    Scheduler scheduler,
    const std::filesystem::path& directory,
    Counters& counters) {
    const std::string payload(FILE_PAYLOAD_SIZE, 'f');
    TaskGroup group {};

    for (int workerIndex { 0 }; workerIndex < FILE_WORKERS; ++workerIndex) {
        group.spawn(run_blocking(blockingWorkers, scheduler, [&, workerIndex] {
            const auto file = directory /
                std::format("worker-{}.bin", workerIndex);

            for (int round { 0 }; round < FILE_ROUNDS; ++round) {
                try {
                    {
                        std::ofstream output {
                            file,
                            std::ios::binary | std::ios::trunc
                        };
                        output.write(payload.data(), payload.size());
                        if (!output) {
                            throw std::runtime_error { "file write failed" };
                        }
                    }

                    std::ifstream input { file, std::ios::binary };
                    std::string actual(
                        std::istreambuf_iterator<char> { input },
                        std::istreambuf_iterator<char> {});
                    if (input.bad() || actual != payload) {
                        throw std::runtime_error { "file verification failed" };
                    }
                    counters.successes_.fetch_add(1);
                } catch (...) {
                    counters.unexpectedFailures_.fetch_add(1);
                }
            }

            const auto missing = directory / "missing" / "input.bin";
            for (int failure { 0 }; failure < FILE_FAILURES; ++failure) {
                std::ifstream input { missing, std::ios::binary };
                if (!input) {
                    counters.expectedFailures_.fetch_add(1);
                } else {
                    counters.unexpectedFailures_.fetch_add(1);
                }
            }
        }));
    }

    co_await group.join();
}

Task<void> run_network_worker(
    IoContext& ioContext,
    BlockingScheduler blockingWorkers,
    Scheduler scheduler,
    std::array<std::byte, NETWORK_PAYLOAD_SIZE> payload,
    Counters& counters) {
    auto [listener, port] = bind_loopback_socket();

    if (::listen(listener.get(), 1) != 0) {
        throw_socket_error("listen");
    }

    // 服务端继续在线程池阻塞；客户端由 IoContext 原生异步驱动。
    TaskGroup serverGroup {};
    serverGroup.spawn(run_blocking(
        blockingWorkers,
        scheduler,
        [listenerHandle = listener.get(), payload] {
            run_echo_server(listenerHandle, payload);
        }));

    int completed {};

    try {
        auto stream = co_await TcpStream::connect(
            ioContext,
            scheduler,
            "127.0.0.1",
            port);
        std::array<std::byte, NETWORK_PAYLOAD_SIZE> response {};

        for (; completed < NETWORK_ROUNDS; ++completed) {
            co_await stream.write_all(scheduler, payload);
            std::size_t received {};

            while (received < response.size()) {
                const auto size = co_await stream.read_some(
                    scheduler,
                    std::span { response }.subspan(received));

                if (size == 0) {
                    throw std::runtime_error { "unexpected server EOF" };
                }

                received += size;
            }

            if (response != payload) {
                throw std::runtime_error { "client payload mismatch" };
            }

            counters.successes_.fetch_add(1);
        }
    } catch (...) {
        counters.unexpectedFailures_.fetch_add(
            NETWORK_ROUNDS - completed);
        static_cast<void>(::shutdown(listener.get(), SHUT_RDWR));
    }

    bool serverFailed {};

    try {
        co_await serverGroup.join();
    } catch (...) {
        serverFailed = true;
    }

    if (serverFailed && completed == NETWORK_ROUNDS) {
        counters.successes_.fetch_sub(1);
        counters.unexpectedFailures_.fetch_add(1);
    }

    auto [reservation, failurePort] = bind_loopback_socket();

    for (int failure { 0 }; failure < NETWORK_FAILURES; ++failure) {
        try {
            static_cast<void>(co_await TcpStream::connect(
                ioContext,
                scheduler,
                "127.0.0.1",
                failurePort));
            counters.unexpectedFailures_.fetch_add(1);
        } catch (const std::system_error& error) {
            if (error.code() == std::make_error_condition(
                    std::errc::connection_refused)) {
                counters.expectedFailures_.fetch_add(1);
            } else {
                counters.unexpectedFailures_.fetch_add(1);
            }
        } catch (...) {
            counters.unexpectedFailures_.fetch_add(1);
        }
    }
}

Task<void> run_network_io(
    IoContext& ioContext,
    BlockingScheduler blockingWorkers,
    Scheduler scheduler,
    Counters& counters) {
    std::array<std::byte, NETWORK_PAYLOAD_SIZE> payload {};
    payload.fill(std::byte { static_cast<unsigned char>('n') });
    TaskGroup group {};

    for (int remaining { NETWORK_WORKERS }; remaining > 0; --remaining) {
        group.spawn(run_network_worker(
            ioContext,
            blockingWorkers,
            scheduler,
            payload,
            counters));
    }

    co_await group.join();
}

template<typename Scenario>
[[nodiscard]] Metrics measure(
    std::string scenario,
    std::size_t operations,
    Scenario scenarioTask) {
    Counters counters {};
    RunLoop loop {};
    const auto start = Clock::now();

    try {
        loop.run(scenarioTask(loop.get_scheduler(), counters));
    } catch (...) {
        counters.unexpectedFailures_.fetch_add(1);
    }

    const auto elapsed = Clock::now() - start;
    return Metrics {
        std::move(scenario),
        operations,
        counters.successes_.load(),
        counters.expectedFailures_.load(),
        counters.unexpectedFailures_.load(),
        std::chrono::duration<double, std::milli> { elapsed }.count()
    };
}

void print_metrics(const Metrics& metrics) {
    const double throughput = metrics.elapsedMilliseconds_ > 0.0
        ? static_cast<double>(metrics.operations_) * 1'000.0 /
            metrics.elapsedMilliseconds_
        : 0.0;

    std::println(
        "{},{},{},{},{},{:.3f},{:.1f},{}",
        metrics.scenario_,
        metrics.operations_,
        metrics.successes_,
        metrics.expectedFailures_,
        metrics.unexpectedFailures_,
        metrics.elapsedMilliseconds_,
        throughput,
        metrics.passed() ? "PASS" : "FAIL");
}

}  // namespace

int main() {
    TemporaryDirectory directory {};
    IoContext ioContext {};
    ThreadPool blockingWorkers {
        std::max(FILE_WORKERS, NETWORK_WORKERS)
    };
    std::vector<Metrics> results {};
    results.reserve(3);

    results.emplace_back(measure(
        "compute",
        COMPUTE_TASKS,
        [](Scheduler scheduler, Counters& counters) {
            return run_compute(scheduler, counters);
        }));

    results.emplace_back(measure(
        "file_io",
        FILE_WORKERS * (FILE_ROUNDS + FILE_FAILURES),
        [&](Scheduler scheduler, Counters& counters) {
            return run_file_io(
                blockingWorkers.get_scheduler(),
                scheduler,
                directory.path(),
                counters);
        }));

    results.emplace_back(measure(
        "network_loopback",
        NETWORK_WORKERS * (NETWORK_ROUNDS + NETWORK_FAILURES),
        [&](Scheduler scheduler, Counters& counters) {
            return run_network_io(
                ioContext,
                blockingWorkers.get_scheduler(),
                scheduler,
                counters);
        }));

    std::println(
        "environment,hardware_threads={} blocking_workers={} free_space_bytes={}",
        std::thread::hardware_concurrency(),
        blockingWorkers.thread_count(),
        std::filesystem::space(directory.path()).available);
    std::println(
        "scenario,operations,successes,expected_failures,unexpected_failures,elapsed_ms,ops_per_second,status");

    for (const auto& result : results) {
        print_metrics(result);
    }

    return std::ranges::all_of(results, &Metrics::passed) ? 0 : 1;
}
