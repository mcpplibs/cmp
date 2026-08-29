export module mcpplibs.cmp:tcp;

import std;
import asio;
import :cancellation;
import :task;

namespace mcpplibs::cmp::detail {

class TcpSocketState;

class IoContextState final {
private:
    using WorkGuard = decltype(asio::make_work_guard(
        std::declval<asio::io_context&>()));

    asio::io_context ioContext_ {};
    WorkGuard workGuard_ { asio::make_work_guard(ioContext_) };
    std::mutex admissionMutex_ {};
    bool accepting_ { true };
    // ponytail: v1 线性清理弱引用；实测注册或关闭成本显著后再换索引结构。
    std::vector<std::weak_ptr<TcpSocketState>> sockets_ {};

    void shutdown_on_driver_() noexcept;

    friend class TcpSocketState;

public:
    template<typename Handler>
    void post(Handler&& handler) {
        const std::lock_guard lock { admissionMutex_ };

        if (!accepting_) {
            throw std::logic_error { "I/O context is stopping" };
        }

        asio::post(ioContext_, std::forward<Handler>(handler));
    }

    void register_socket_on_driver(
        const std::shared_ptr<TcpSocketState>& socket) {
        std::erase_if(sockets_, [](const auto& registered) {
            return registered.expired();
        });
        sockets_.push_back(socket);
    }

    void run_driver() noexcept {
        try {
            ioContext_.run();
        } catch (...) {
            std::terminate();
        }
    }

    void begin_shutdown() noexcept {
        const std::lock_guard lock { admissionMutex_ };
        accepting_ = false;
        asio::post(ioContext_, [this] {
            shutdown_on_driver_();
        });
    }
};

class TcpSocketState final {
private:
    std::weak_ptr<IoContextState> context_ {};
    std::optional<asio::ip::tcp::socket> socket_ {};

public:
    explicit TcpSocketState(
        const std::shared_ptr<IoContextState>& context)
        : context_ { context },
          socket_ { std::in_place, context->ioContext_ } {}

    void close_on_driver() noexcept {
        if (!socket_) {
            return;
        }

        std::error_code ignored {};
        socket_->close(ignored);
        socket_.reset();
    }
};

void IoContextState::shutdown_on_driver_() noexcept {
    for (auto& registered : sockets_) {
        if (const auto socket = registered.lock()) {
            socket->close_on_driver();
        }
    }

    sockets_.clear();
    workGuard_.reset();
}

}  // namespace mcpplibs::cmp::detail

export namespace mcpplibs::cmp {

class IoContext final {
private:
    // 共享状态必须先构造、后析构，driver 才能安全排空并退出。
    std::shared_ptr<detail::IoContextState> state_ {
        std::make_shared<detail::IoContextState>()
    };
    std::jthread driver_ { [state = state_] {
        state->run_driver();
    } };

public:
    IoContext() = default;

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    ~IoContext() {
        if (driver_.get_id() == std::this_thread::get_id()) {
            std::terminate();
        }

        state_->begin_shutdown();
        driver_.join();
    }
};

}  // namespace mcpplibs::cmp
