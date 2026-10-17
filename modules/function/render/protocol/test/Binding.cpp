#include <lux/engine/render/protocol/CommandHandler.hpp>
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

static RenderResult<void> apply(State& state, const Command::Request& input, Reply<Command>& context) noexcept
{
    state.value = input.value;
    return context.complete(unsigned(input.value));
}

static RenderResult<void> wrong(State&, const OtherRequest&, Reply<Command>&) noexcept { return {}; }
static RenderResult<void> throwing(State&, const Command::Request&, Reply<Command>&) { return {}; }

template <auto Function>
concept CanBind = requires(State& state) { bindCommand<Command, Function>(state); };
static_assert(CanBind<apply>);
static_assert(!CanBind<wrong> && !CanBind<throwing>);
static_assert(sizeof(OtherRequest) == sizeof(Command::Request));

struct MemberHandler final
{
    unsigned calls = 0;

    void apply(const Command::Request& input, Reply<Command> reply) noexcept
    {
        ++calls;
        if (!reply.complete(unsigned(input.value + 1))) std::abort();
    }

    void wrong(const OtherRequest&, Reply<Command>) noexcept {}
    void throwing(const Command::Request&, Reply<Command>) {}
};

template <auto Method>
concept CanBindMember = requires(MemberHandler& handler) { bindCommand<Command, Method>(handler); };
static_assert(CanBindMember<&MemberHandler::apply>);
static_assert(!CanBindMember<&MemberHandler::wrong> && !CanBindMember<&MemberHandler::throwing>);
static_assert(!std::is_same_v<HandlerId, CommandId>);

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
    MemberHandler member;
    const auto bound = bindCommand<Command, &MemberHandler::apply>(member);
    if (!bound.invoke(invocation) || member.calls != 1 || result != 48) std::abort();
    if (bound.call().object != &member || sizeof(HandlerCall) != 2 * sizeof(void*)) std::abort();
}
