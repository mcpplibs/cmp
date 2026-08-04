import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;

class InlineRunner {
public:
    struct promise_type {
        [[nodiscard]] InlineRunner get_return_object() const noexcept;

        [[nodiscard]] constexpr std::suspend_never initial_suspend() const noexcept {
            return {};
        }

        [[nodiscard]] constexpr std::suspend_never final_suspend() const noexcept {
            return {};
        }

        constexpr void return_void() const noexcept {}

        [[noreturn]] void unhandled_exception() const noexcept {
            std::terminate();
        }
    };
};

InlineRunner InlineRunner::promise_type::get_return_object() const noexcept {
    return {};
}

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer() {
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}

InlineRunner run_inline(Task<void> task) {
    co_await std::move(task);
    co_return;
}

int main() {
    run_inline(print_answer());
    return 0;
}
