#include <lux/engine/render/protocol/Protocol.hpp>
#include <cstdlib>

using namespace lux::render;

struct Command final
{
    static constexpr std::string_view name = "test.typed.binding";
    struct Request { unsigned value; };
    using Response = unsigned;
};
struct OtherRequest { unsigned value; };
struct State { unsigned value = 0; };

static RenderResult<void> apply(State& state, const Command::Request& input, CommandContext<Command>& context) noexcept
{
    state.value = input.value;
    return context.complete(unsigned(input.value));
}

static RenderResult<void> wrong(State&, const OtherRequest&, CommandContext<Command>&) noexcept { return {}; }
static RenderResult<void> throwing(State&, const Command::Request&, CommandContext<Command>&) { return {}; }

template <auto Function>
concept CanBind = requires(State& state) { bindCommand<Command, Function>(state); };
static_assert(CanBind<apply>);
static_assert(!CanBind<wrong> && !CanBind<throwing>);
static_assert(sizeof(OtherRequest) == sizeof(Command::Request));

int main()
{
    State state;
    unsigned result = 0;
    const CommandCompletionFns completion{
        [](void* output, ECommandOutcome outcome, RenderError, void* value) noexcept -> RenderResult<void> {
            if (outcome != ECommandOutcome::SUCCESS) std::abort();
            *static_cast<unsigned*>(output) = *static_cast<unsigned*>(value);
            return {};
        }, [](const void*) noexcept { return true; }
    };
    const auto handler = bindCommand<Command, apply>(state);
    const Command::Request input{47};
    CommandInvocation invocation{&input, &result, &completion};
    if (!handler.invoke(invocation) || state.value != 47 || result != 47) std::abort();
}
