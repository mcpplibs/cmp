import std;
import mcpplibs.cmp;

namespace {

using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::Task;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::ThreadPool;
using mcpplibs::cmp::when_all;

using Scheduler = ThreadPool::Scheduler;
using Clock = std::chrono::steady_clock;

constexpr int CPU_TASK_COUNT { 512 };
constexpr int CPU_STEPS { 200'000 };
constexpr int SCHEDULE_HOPS { 200'000 };
constexpr int NESTED_TASKS { 20'000 };
constexpr int PRODUCER_COUNT { 8 };
constexpr int HOPS_PER_PRODUCER { 25'000 };

struct Counters final {
    std::atomic<std::size_t> successes_ {};
    std::atomic<std::size_t> unexpectedFailures_ {};
    std::atomic<std::size_t> active_ {};
    std::atomic<std::size_t> maxConcurrency_ {};
};

struct Metrics final {
    std::string scenario_ {};
    std::size_t workerCount_ {};
    std::size_t operations_ {};
    std::size_t successes_ {};
    std::size_t unexpectedFailures_ {};
    std::size_t maxConcurrency_ {};
    double elapsedMilliseconds_ {};

    [[nodiscard]] bool passed() const noexcept {
        return successes_ == operations_ && unexpectedFailures_ == 0;
    }
};

void record_completion(Counters& counters) noexcept {
    const auto active = counters.active_.fetch_add(
        1,
        std::memory_order_relaxed) + 1;
    auto maximum = counters.maxConcurrency_.load(std::memory_order_relaxed);

    while (maximum < active &&
           !counters.maxConcurrency_.compare_exchange_weak(
               maximum,
               active,
               std::memory_order_relaxed)) {
    }

    counters.successes_.fetch_add(1, std::memory_order_relaxed);
    counters.active_.fetch_sub(1, std::memory_order_relaxed);
}

[[nodiscard]] std::uint64_t compute_value(int index) noexcept {
    std::uint64_t value = static_cast<std::uint64_t>(index) + 1;

    for (int step { 0 }; step < CPU_STEPS; ++step) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
    }

    return value;
}

Task<void> compute_operation(
    Scheduler scheduler,
    int index,
    Counters& counters,
    std::atomic<std::uint64_t>& checksum) {
    co_await scheduler.schedule();

    const auto active = counters.active_.fetch_add(
        1,
        std::memory_order_relaxed) + 1;
    auto maximum = counters.maxConcurrency_.load(std::memory_order_relaxed);
    while (maximum < active &&
           !counters.maxConcurrency_.compare_exchange_weak(
               maximum,
               active,
               std::memory_order_relaxed)) {
    }

    checksum.fetch_xor(compute_value(index), std::memory_order_relaxed);
    counters.successes_.fetch_add(1, std::memory_order_relaxed);
    counters.active_.fetch_sub(1, std::memory_order_relaxed);
}

Task<void> run_compute(
    Scheduler scheduler,
    Counters& counters,
    std::uint64_t expectedChecksum) {
    std::atomic<std::uint64_t> checksum {};
    TaskGroup group {};

    for (int index { 0 }; index < CPU_TASK_COUNT; ++index) {
        group.spawn(compute_operation(
            scheduler,
            index,
            counters,
            checksum));
    }

    co_await group.join();
    if (checksum.load(std::memory_order_relaxed) != expectedChecksum) {
        counters.unexpectedFailures_.fetch_add(1);
    }
}

Task<void> run_schedule_hops(
    Scheduler scheduler,
    Counters& counters) {
    for (int index { 0 }; index < SCHEDULE_HOPS; ++index) {
        co_await scheduler.schedule();
        record_completion(counters);
    }
}

Task<void> run_one_shot(
    Scheduler scheduler,
    Counters& counters) {
    co_await scheduler.schedule();
    record_completion(counters);
}

Task<void> run_nested_fanout(
    Scheduler scheduler,
    Counters& counters) {
    co_await scheduler.schedule();
    std::vector<Task<void>> tasks {};
    tasks.reserve(NESTED_TASKS);

    for (int index { 0 }; index < NESTED_TASKS; ++index) {
        tasks.emplace_back(run_one_shot(scheduler, counters));
    }

    static_cast<void>(co_await when_all(std::move(tasks)));
}

Task<void> run_producer(
    Scheduler scheduler,
    Counters& counters) {
    for (int index { 0 }; index < HOPS_PER_PRODUCER; ++index) {
        co_await scheduler.schedule();
        record_completion(counters);
    }
}

Task<void> run_concurrent_producers(
    Scheduler scheduler,
    Counters& counters) {
    TaskGroup group {};

    for (int index { 0 }; index < PRODUCER_COUNT; ++index) {
        group.spawn(run_producer(scheduler, counters));
    }

    co_await group.join();
}

template<typename Scenario>
[[nodiscard]] Metrics measure(
    std::string scenario,
    ThreadPool& pool,
    std::size_t operations,
    Scenario scenarioTask) {
    Counters counters {};
    RunLoop loop {};
    const auto start = Clock::now();

    try {
        loop.run(scenarioTask(pool.get_scheduler(), counters));
    } catch (...) {
        counters.unexpectedFailures_.fetch_add(1);
    }

    const auto elapsed = Clock::now() - start;
    return Metrics {
        std::move(scenario),
        pool.thread_count(),
        operations,
        counters.successes_.load(),
        counters.unexpectedFailures_.load(),
        counters.maxConcurrency_.load(),
        std::chrono::duration<double, std::milli> { elapsed }.count()
    };
}

void print_metrics(const Metrics& metrics) {
    const double throughput = metrics.elapsedMilliseconds_ > 0.0
        ? static_cast<double>(metrics.operations_) * 1'000.0 /
            metrics.elapsedMilliseconds_
        : 0.0;

    std::println(
        "{},{},{},{},{},{},{:.3f},{:.1f},{}",
        metrics.scenario_,
        metrics.workerCount_,
        metrics.operations_,
        metrics.successes_,
        metrics.unexpectedFailures_,
        metrics.maxConcurrency_,
        metrics.elapsedMilliseconds_,
        throughput,
        metrics.passed() ? "PASS" : "FAIL");
}

[[nodiscard]] std::vector<std::size_t> worker_counts() {
    const auto hardware = std::max<std::size_t>(
        1,
        std::thread::hardware_concurrency());
    const auto maximum = std::min<std::size_t>(hardware, 8);
    std::vector<std::size_t> counts {};

    for (const std::size_t candidate : { 1U, 2U, 4U, 8U }) {
        if (candidate <= maximum) {
            counts.push_back(candidate);
        }
    }

    if (counts.back() != maximum) {
        counts.push_back(maximum);
    }

    return counts;
}

[[nodiscard]] std::uint64_t expected_checksum() noexcept {
    std::uint64_t checksum {};

    for (int index { 0 }; index < CPU_TASK_COUNT; ++index) {
        checksum ^= compute_value(index);
    }

    return checksum;
}

}  // namespace

int main() {
    const auto expectedChecksum = expected_checksum();
    std::vector<Metrics> results {};

    for (const auto workerCount : worker_counts()) {
        ThreadPool pool { workerCount };

        results.emplace_back(measure(
            "cpu_chunks",
            pool,
            CPU_TASK_COUNT,
            [&](Scheduler scheduler, Counters& counters) {
                return run_compute(
                    scheduler,
                    counters,
                    expectedChecksum);
            }));
        results.emplace_back(measure(
            "schedule_hops",
            pool,
            SCHEDULE_HOPS,
            run_schedule_hops));
        results.emplace_back(measure(
            "nested_fanout",
            pool,
            NESTED_TASKS,
            run_nested_fanout));
        results.emplace_back(measure(
            "concurrent_reschedule",
            pool,
            PRODUCER_COUNT * HOPS_PER_PRODUCER,
            run_concurrent_producers));
    }

    std::println(
        "environment,hardware_threads={}",
        std::thread::hardware_concurrency());
    std::println(
        "scenario,workers,operations,successes,unexpected_failures,max_concurrency,elapsed_ms,ops_per_second,status");

    for (const auto& result : results) {
        print_metrics(result);
    }

    return std::ranges::all_of(results, &Metrics::passed) ? 0 : 1;
}
