import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::OperationCancelled;

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

Task<void> print_cancellation(RunLoop::Scheduler scheduler, std::stop_token token) {
    try {
        co_await scheduler.schedule_after(1s, token);
    } catch (const OperationCancelled&) {
        std::println("Coroutine cancelled");
    }
    co_return;
}

int main() {
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));

    std::stop_source source {};
    source.request_stop();
    loop.run(print_cancellation(loop.get_scheduler(), source.get_token()));
    return 0;
}
