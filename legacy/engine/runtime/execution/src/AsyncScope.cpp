#include <lux/engine/runtime/execution/AsyncScope.hpp>

#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadScheduler.hpp>

#include <exec/async_scope.hpp>
#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdint>
#include <new>

namespace lux::exec::detail
{
    namespace
    {
        enum class EScopeState : std::uint8_t
        {
            OPEN,
            CLOSING,
            CLOSED
        };

        constexpr std::uint64_t kStateBits = 2u;
        constexpr std::uint64_t kStateMask =
            (std::uint64_t{1u} << kStateBits) - 1u;

        [[nodiscard]] constexpr std::uint64_t encode(
            EScopeState state,
            std::uint64_t producers) noexcept
        {
            return (producers << kStateBits) |
                static_cast<std::uint64_t>(state);
        }

        [[nodiscard]] constexpr EScopeState decodeState(
            std::uint64_t gate) noexcept
        {
            return static_cast<EScopeState>(gate & kStateMask);
        }

        [[nodiscard]] constexpr std::uint64_t producerCount(
            std::uint64_t gate) noexcept
        {
            return gate >> kStateBits;
        }

        struct CloseWaiter final
        {
            explicit CloseWaiter(
                lux::cxx::move_only_function<void()> value) noexcept
                : completion(std::move(value))
            {}

            CloseWaiter* next{nullptr};
            lux::cxx::move_only_function<void()> completion;
        };

        [[nodiscard]] CloseWaiter* completedSentinel() noexcept
        {
            return reinterpret_cast<CloseWaiter*>(std::uintptr_t{1u});
        }
    }

    class AsyncScopeState final
        : public std::enable_shared_from_this<AsyncScopeState>
    {
    public:
        explicit AsyncScopeState(AsyncRuntime& runtime_value) noexcept
            : runtime(&runtime_value)
        {}

        void requestClose() noexcept
        {
            auto gate = admission_gate.load(std::memory_order_acquire);
            while (decodeState(gate) == EScopeState::OPEN)
            {
                if (admission_gate.compare_exchange_weak(
                        gate,
                        encode(EScopeState::CLOSING, producerCount(gate)),
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    (void)scope.request_stop();
                    break;
                }
            }
            tryStartWaiter();
        }

        [[nodiscard]] bool tryAcquire() noexcept
        {
            auto gate = admission_gate.load(std::memory_order_acquire);
            while (decodeState(gate) == EScopeState::OPEN)
            {
                if (admission_gate.compare_exchange_weak(
                        gate,
                        gate + (std::uint64_t{1u} << kStateBits),
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    used.store(true, std::memory_order_release);
                    return true;
                }
            }
            return false;
        }

        void releaseAdmission() noexcept
        {
            const auto previous = admission_gate.fetch_sub(
                std::uint64_t{1u} << kStateBits,
                std::memory_order_acq_rel);
            if (producerCount(previous) == 1u &&
                decodeState(previous) == EScopeState::CLOSING)
                tryStartWaiter();
        }

        void subscribe(
            lux::cxx::move_only_function<void()> completion) noexcept
        {
            auto* waiter = new (std::nothrow) CloseWaiter{
                std::move(completion)};
            if (waiter == nullptr)
                std::terminate();

            auto* head = waiters.load(std::memory_order_acquire);
            for (;;)
            {
                if (head == completedSentinel())
                {
                    auto terminal = std::move(waiter->completion);
                    delete waiter;
                    terminal();
                    return;
                }
                waiter->next = head;
                if (waiters.compare_exchange_weak(
                        head,
                        waiter,
                        std::memory_order_release,
                        std::memory_order_acquire))
                    break;
            }
            requestClose();
        }

        [[nodiscard]] bool isOpen() const noexcept
        {
            return decodeState(
                admission_gate.load(std::memory_order_acquire)) ==
                EScopeState::OPEN;
        }

        void tryStartWaiter() noexcept
        {
            const auto gate = admission_gate.load(std::memory_order_acquire);
            if (decodeState(gate) != EScopeState::CLOSING ||
                producerCount(gate) != 0u ||
                waiter_started.exchange(true, std::memory_order_acq_rel))
                return;

            if (!used.load(std::memory_order_acquire))
            {
                complete();
                return;
            }

            auto self = shared_from_this();
            auto waiter = scope.on_empty()
                | stdexec::continues_on(MainThreadScheduler{runtime->mainThreadMailbox()})
                | stdexec::then(
                      [self = std::move(self)]() noexcept
                      {
                          self->complete();
                      });
            ::experimental::execution::start_detached(std::move(waiter));
        }

        void complete() noexcept
        {
            admission_gate.store(
                encode(EScopeState::CLOSED, 0u),
                std::memory_order_release);
            auto* waiter = waiters.exchange(
                completedSentinel(), std::memory_order_acq_rel);
            while (waiter != nullptr && waiter != completedSentinel())
            {
                auto* next = waiter->next;
                auto completion = std::move(waiter->completion);
                delete waiter;
                completion();
                waiter = next;
            }
        }

        AsyncRuntime* runtime{nullptr};
        ::exec::async_scope scope;
        std::atomic<std::uint64_t> admission_gate{
            encode(EScopeState::OPEN, 0u)};
        std::atomic<bool> used{false};
        std::atomic<bool> waiter_started{false};
        std::atomic<CloseWaiter*> waiters{nullptr};
    };

    void releaseScopeAdmission(
        const std::shared_ptr<AsyncScopeState>& state) noexcept
    {
        if (state)
            state->releaseAdmission();
    }

    void subscribeScopeClose(
        std::shared_ptr<AsyncScopeState> state,
        lux::cxx::move_only_function<void()> completion) noexcept
    {
        if (!state)
        {
            completion();
            return;
        }
        state->subscribe(std::move(completion));
    }

    void subscribeScopeClose(
        AsyncScope& scope,
        lux::cxx::move_only_function<void()> completion) noexcept
    {
        subscribeScopeClose(scope.state_, std::move(completion));
    }
}

namespace lux::exec
{
    AsyncScope::AsyncScope(AsyncRuntime& runtime) noexcept
        : state_(std::make_shared<detail::AsyncScopeState>(runtime))
    {}

    AsyncScope::~AsyncScope()
    {
        if (state_)
            state_->requestClose();
    }

    void AsyncScope::requestStop() noexcept
    {
        if (state_)
            state_->requestClose();
    }

    AsyncScopeCloseSender AsyncScope::closeAsync() noexcept
    {
        return AsyncScopeCloseSender{state_};
    }

    bool AsyncScope::isOpen() const noexcept
    {
        return state_ && state_->isOpen();
    }

    AsyncScope::AdmissionTicket AsyncScope::tryAcquireAdmission() noexcept
    {
        if (!state_ || !state_->tryAcquire())
            return {};
        return AdmissionTicket{state_};
    }

    ::experimental::execution::async_scope& AsyncScope::asyncScope() noexcept
    {
        if (!state_)
            std::terminate();
        return state_->scope;
    }
}
