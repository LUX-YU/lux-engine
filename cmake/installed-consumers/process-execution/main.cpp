#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/PortSender.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/Timer.hpp>

#include <exec/materialize.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <utility>

namespace
{
    enum class EConsumerError : std::uint8_t
    {
        FAILED
    };

    struct Read final
    {
        using Value = int;
        using Error = EConsumerError;

        int value{};
    };

    class Endpoint final : public lux::async::detail::OperationEndpoint<Read>
    {
    public:
        [[nodiscard]] lux::async::SubmitResult submit(
            Read operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions
        ) noexcept override
        {
            complete(state, Outcome{operation.value});
            return {};
        }
    };

    struct TimerReceiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value() && noexcept
        {
            completed->store(true, std::memory_order_release);
            completed->notify_all();
        }

        void set_error(lux::process::ETimerError) && noexcept
        {
            failed->store(true, std::memory_order_release);
            std::move(*this).set_value();
        }

        void set_stopped() && noexcept
        {
            failed->store(true, std::memory_order_release);
            std::move(*this).set_value();
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        std::atomic_bool* completed{};
        std::atomic_bool* failed{};
    };

    struct PortReceiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value(int received) && noexcept
        {
            *value = received;
        }

        void set_error(lux::async::OperationFailure<EConsumerError>) && noexcept
        {
            *failed = true;
        }

        void set_stopped() && noexcept
        {
            *failed = true;
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        int* value{};
        bool* failed{};
    };
}

int main()
{
    auto created = lux::process::TimerQueue::create({1U});
    if (!created)
        return 1;
    auto timer = std::move(*created);
    std::atomic_bool completed{};
    std::atomic_bool timer_failed{};
    auto timer_state = stdexec::connect(
        timer.client().after(std::chrono::steady_clock::duration::zero()),
        TimerReceiver{&completed, &timer_failed}
    );
    stdexec::start(timer_state);
    completed.wait(false, std::memory_order_acquire);
    if (timer_failed.load(std::memory_order_acquire))
        return 2;

    auto endpoint = std::make_shared<Endpoint>();
    lux::async::OperationPort<Read> port{endpoint};
    int value{};
    bool port_failed{};
    auto port_state = stdexec::connect(
        lux::process::portSender(port, Read{42}),
        PortReceiver{&value, &port_failed}
    );
    stdexec::start(port_state);
    if (value != 42 || port_failed)
        return 3;

    auto runtime_created = lux::process::ExecutionRuntime::create({2U, 4U, 4U, {4U}});
    if (!runtime_created)
        return 4;
    auto runtime = std::move(*runtime_created);
    lux::process::TaskScope scope;
    std::atomic_bool cpu_ran{};
    std::atomic_bool cpu_failed{};
    auto scheduled = stdexec::schedule(runtime.cpu()) |
        stdexec::then([&cpu_ran]() noexcept {
            cpu_ran.store(true, std::memory_order_release);
            cpu_ran.notify_all();
        });
    auto cpu_work = exec::materialize(std::move(scheduled)) |
        stdexec::then([&cpu_failed]<class Tag, class... Values>(Tag, Values&&...) noexcept {
            if constexpr (!std::same_as<Tag, stdexec::set_value_t>)
                cpu_failed.store(true, std::memory_order_release);
        });
    if (!scope.start(std::move(cpu_work)))
        return 5;
    cpu_ran.wait(false, std::memory_order_acquire);
    const auto closed = stdexec::sync_wait(scope.close());
    if (!closed || !cpu_ran.load(std::memory_order_acquire) || cpu_failed.load(std::memory_order_acquire))
        return 6;
    runtime.requestStop();
    return runtime.join() ? 0 : 7;
}
