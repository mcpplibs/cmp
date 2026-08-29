import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::AsyncManualResetEvent;
using mcpplibs::cmp::AsyncMutex;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::OneShotEvent;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::ThreadPool;
using mcpplibs::cmp::run_blocking;
using mcpplibs::cmp::when_all;

using namespace std::chrono_literals;

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer(RunLoop::Scheduler scheduler) {
    co_await scheduler.schedule_after(10ms);
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}

Task<int> delayed_value(
    RunLoop::Scheduler scheduler,
    std::chrono::milliseconds delay,
    int value) {
    co_await scheduler.schedule_after(delay);
    co_return value;
}

Task<void> print_concurrent_results(RunLoop::Scheduler scheduler) {
    std::vector<Task<int>> tasks {};
    tasks.emplace_back(delayed_value(scheduler, 10ms, 20));
    tasks.emplace_back(delayed_value(scheduler, 1ms, 22));
    auto values = co_await when_all(std::move(tasks));
    std::println("Concurrent result: {}", values[0] + values[1]);
    co_return;
}

Task<int> calculate_on_workers(
    ThreadPool::Scheduler workers,
    RunLoop::Scheduler caller) {
    co_await workers.schedule();

    int result { 0 };
    for (int value { 1 }; value <= 6; ++value) {
        result += value * 2;
    }

    co_await caller.schedule();
    co_return result;
}

Task<void> print_worker_result(
    ThreadPool::Scheduler workers,
    RunLoop::Scheduler caller) {
    const auto result = co_await calculate_on_workers(workers, caller);
    std::println("Worker pool result: {}", result);
}

Task<void> print_blocking_result(
    ThreadPool::Scheduler blockingWorkers,
    RunLoop::Scheduler caller) {
    const auto result = co_await run_blocking(
        blockingWorkers,
        caller,
        [] {
            std::this_thread::sleep_for(1ms);
            return 42;
        });
    std::println("Blocking result: {}", result);
}

Task<void> add_delayed(
    RunLoop::Scheduler scheduler,
    std::chrono::milliseconds delay,
    int value,
    int& total) {
    co_await scheduler.schedule_after(delay);
    total += value;
    co_return;
}

Task<void> print_task_group(RunLoop::Scheduler scheduler) {
    int total { 0 };
    TaskGroup group {};
    group.spawn(add_delayed(scheduler, 10ms, 20, total));
    group.spawn(add_delayed(scheduler, 1ms, 22, total));
    co_await group.join();
    std::println("Task group result: {}", total);
    co_return;
}

Task<void> add_recursively(
    RunLoop::Scheduler scheduler,
    TaskGroup& group,
    int remaining,
    int& total) {
    co_await scheduler.schedule();
    ++total;

    if (remaining > 1) {
        group.spawn(add_recursively(
            scheduler,
            group,
            remaining - 1,
            total));
    }
    co_return;
}

Task<void> print_recursive_group(RunLoop::Scheduler scheduler) {
    int total { 0 };
    TaskGroup group {};
    group.spawn(add_recursively(scheduler, group, 3, total));
    co_await group.join();
    std::println("Recursive group result: {}", total);
    co_return;
}

Task<void> set_event(
    RunLoop::Scheduler scheduler,
    OneShotEvent& event) {
    co_await scheduler.schedule();
    event.set();
    co_return;
}

Task<void> print_event(RunLoop::Scheduler scheduler) {
    OneShotEvent event {};
    TaskGroup group {};
    group.spawn(set_event(scheduler, event));
    co_await event;
    co_await group.join();
    std::println("Event signalled");
    co_return;
}

Task<void> set_manual_event(
    RunLoop::Scheduler scheduler,
    AsyncManualResetEvent& event) {
    co_await scheduler.schedule();
    event.set();
    co_return;
}

Task<void> print_manual_reset_event(RunLoop::Scheduler scheduler) {
    AsyncManualResetEvent event {};

    for (int cycle { 0 }; cycle < 2; ++cycle) {
        TaskGroup group {};
        group.spawn(set_manual_event(scheduler, event));
        co_await event;
        co_await group.join();
        event.reset();
    }

    std::println("Reusable event cycles: 2");
    co_return;
}

Task<void> add_locked(
    RunLoop::Scheduler scheduler,
    AsyncMutex& mutex,
    int value,
    int& total) {
    co_await scheduler.schedule();
    auto guard = co_await mutex.lock_async();
    total += value;
    co_return;
}

Task<void> print_mutex(RunLoop::Scheduler scheduler) {
    int total { 0 };
    AsyncMutex mutex {};
    TaskGroup group {};
    group.spawn(add_locked(scheduler, mutex, 20, total));
    group.spawn(add_locked(scheduler, mutex, 22, total));
    co_await group.join();
    std::println("Mutex result: {}", total);
    co_return;
}

Task<void> observe_group_cancellation(
    RunLoop::Scheduler scheduler,
    std::stop_token token,
    bool& cancelled) {
    try {
        co_await scheduler.schedule(token);
    } catch (const OperationCancelled&) {
        cancelled = true;
    }
    co_return;
}

Task<void> print_cancellation(RunLoop::Scheduler scheduler) {
    bool cancelled { false };
    TaskGroup group {};
    group.spawn(observe_group_cancellation(
        scheduler,
        group.get_stop_token(),
        cancelled));
    co_await group.cancel_and_join();

    if (cancelled) {
        std::println("Coroutine cancelled");
    }
    co_return;
}

int main() {
    ThreadPool workers { 2 };
    ThreadPool blockingWorkers { 2 };
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
    loop.run(print_concurrent_results(loop.get_scheduler()));
    loop.run(print_worker_result(
        workers.get_scheduler(),
        loop.get_scheduler()));
    loop.run(print_blocking_result(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler()));
    loop.run(print_task_group(loop.get_scheduler()));
    loop.run(print_recursive_group(loop.get_scheduler()));
    loop.run(print_event(loop.get_scheduler()));
    loop.run(print_manual_reset_event(loop.get_scheduler()));
    loop.run(print_mutex(loop.get_scheduler()));
    loop.run(print_cancellation(loop.get_scheduler()));
    return 0;
}
