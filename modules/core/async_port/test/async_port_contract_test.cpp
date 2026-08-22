#include <lux/engine/core/async/OperationPort.hpp>

#include <cassert>
#include <memory>
#include <type_traits>

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

    class Endpoint final
        : public lux::async::detail::OperationEndpoint<Notify>
    {
    public:
        [[nodiscard]] lux::async::SubmitResult submit(
            Notify operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions) noexcept override
        {
            observed = operation.value;
            complete(state, Outcome{});
            return {};
        }

        int observed{0};
    };
}

int main()
{
    static_assert(lux::async::Operation<Notify>);
    static_assert(std::is_copy_constructible_v<
        lux::async::OperationPort<Notify>>);

    lux::async::OperationPort<Notify> missing;
    const auto rejected = missing.tryNotify(Notify{1});
    assert(!rejected);
    assert(rejected.error() == lux::async::ESubmitError::UNKNOWN_OPERATION);

    auto endpoint = std::make_shared<Endpoint>();
    lux::async::OperationPort<Notify> port{endpoint};
    assert(port.tryNotify(Notify{42}));
    assert(endpoint->observed == 42);
    assert(lux::async::operationType<Notify>().isValid());
}
