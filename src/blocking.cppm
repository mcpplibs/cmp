export module mcpplibs.cmp:blocking;

import std;
import :task;
import :thread_pool;

export namespace mcpplibs::cmp {

template<typename ReturnScheduler, typename Function>
requires (
    std::move_constructible<Function> &&
    std::invocable<Function> &&
    (
        std::same_as<std::invoke_result_t<Function>, void> ||
        (
            std::is_object_v<std::invoke_result_t<Function>> &&
            !std::is_array_v<std::invoke_result_t<Function>> &&
            std::move_constructible<std::invoke_result_t<Function>>
        )
    ) &&
    std::move_constructible<ReturnScheduler> &&
    requires(const ReturnScheduler& scheduler) {
        scheduler.schedule();
    }
)
[[nodiscard]] Task<std::invoke_result_t<Function>> run_blocking(
    ThreadPool::Scheduler blockingWorkers,
    ReturnScheduler returnTo,
    Function operation,
    std::stop_token stopToken = {}) {
    using Result = std::invoke_result_t<Function>;

    std::exception_ptr exception {};

    if constexpr (std::same_as<Result, void>) {
        try {
            co_await blockingWorkers.schedule(std::move(stopToken));
            std::invoke(std::move(operation));
        } catch (...) {
            exception = std::current_exception();
        }

        // 返回路径不可取消，否则结果会滞留在 blocking worker。
        co_await returnTo.schedule();

        if (exception) {
            std::rethrow_exception(exception);
        }

        co_return;
    } else {
        std::optional<Result> result {};

        try {
            co_await blockingWorkers.schedule(std::move(stopToken));
            result.emplace(std::invoke(std::move(operation)));
        } catch (...) {
            exception = std::current_exception();
        }

        // 返回后再发布值或异常，调用方始终在显式目标 Scheduler 上继续。
        co_await returnTo.schedule();

        if (exception) {
            std::rethrow_exception(exception);
        }

        co_return std::move(*result);
    }
}

}  // namespace mcpplibs::cmp
