import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;

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

int main() {
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
    return 0;
}
