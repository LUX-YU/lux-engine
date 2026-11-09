#include <exec/task.hpp>
#include <lux/engine/core/execution/ExecutionResult.hpp>
#include <lux/engine/object/SignalSender.hpp>

namespace
{
    class Source final : public lux::object::LuxObject
    {
    public:
        lux::object::TSignal<int> changed{*this};
        lux::object::TSignal<> ready{*this};
    };

    [[maybe_unused]] exec::task<lux::execution::ExecutionResult<int, lux::object::EConnectError>>
    receiveValue(Source& source)
    {
        co_return co_await lux::execution::asResult(lux::object::whenEmitted(source, &Source::changed));
    }

    [[maybe_unused]] exec::task<lux::execution::ExecutionResult<void, lux::object::EConnectError>>
    receiveVoid(Source& source)
    {
        co_return co_await lux::execution::asResult(lux::object::whenEmitted(source, &Source::ready));
    }
}
