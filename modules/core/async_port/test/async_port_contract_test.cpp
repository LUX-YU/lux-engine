#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/core/async/OperationInbox.hpp>

#include <cassert>
#include <memory>
#include <type_traits>
#include <utility>

namespace
{
    enum class ETestError : std::uint8_t
    {
        REJECTED
    };

    struct Notify final
    {
        using Value = void;
        using Error = ETestError;

        int value{0};
    };

    class Endpoint final : public lux::async::detail::OperationEndpoint<Notify>
    {
    public:
        [[nodiscard]] lux::async::SubmitResult submit(
            Notify operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions
        ) noexcept override
        {
            observed = operation.value;
            complete(state, Outcome{});
            return {};
        }

        int observed{0};
    };

    class DeferredEndpoint final : public lux::async::detail::OperationEndpoint<Notify>
    {
    public:
        [[nodiscard]] lux::async::SubmitResult submit(
            Notify operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions
        ) noexcept override
        {
            observed = operation.value;
            state_ = state;
            complete_ = complete;
            return {};
        }

        void finish() noexcept
        {
            const auto complete = std::exchange(complete_, nullptr);
            const auto state = std::exchange(state_, nullptr);
            complete(state, Outcome{});
        }

        int observed{0};

    private:
        void* state_{nullptr};
        void (*complete_)(void*, Outcome&&) noexcept {nullptr};
    };
}

int
main()
{
    static_assert(lux::async::Operation<Notify>);
    static_assert(std::is_copy_constructible_v<lux::async::OperationPort<Notify>>);

    lux::async::OperationPort<Notify> missing;
    const auto rejected = missing.tryNotify(Notify{1});
    assert(!rejected);
    assert(rejected.error() == lux::async::ESubmitError::UNKNOWN_OPERATION);

    auto endpoint = std::make_shared<Endpoint>();
    lux::async::OperationPort<Notify> port{endpoint};
    assert(port.tryNotify(Notify{42}));
    assert(endpoint->observed == 42);
    assert(lux::async::operationType<Notify>().isValid());

    lux::async::OperationInbox<Notify, int> rejected_inbox{1u};
    const auto inbox_rejected = rejected_inbox.submit(missing, Notify{2}, 7);
    assert(!inbox_rejected);
    assert(rejected_inbox.terminal());

    lux::async::OperationInbox<Notify, int> immediate_inbox{1u};
    assert(immediate_inbox.submit(port, Notify{43}, 8));
    assert(!immediate_inbox.terminal());
    assert(immediate_inbox.drain([](auto completion) noexcept {
        assert(completion.key == 8);
        assert(completion.outcome);
    }) == 1u);
    assert(immediate_inbox.terminal());

    auto deferred_endpoint = std::make_shared<DeferredEndpoint>();
    lux::async::OperationPort<Notify> deferred_port{deferred_endpoint};
    lux::async::OperationInbox<Notify, int> deferred_inbox{1u};
    assert(deferred_inbox.submit(deferred_port, Notify{44}, 9));
    assert(!deferred_inbox.terminal());
    deferred_endpoint->finish();
    assert(deferred_inbox.drain([](auto completion) noexcept {
        assert(completion.key == 9);
        assert(completion.outcome);
    }) == 1u);
    assert(deferred_inbox.terminal());
}
