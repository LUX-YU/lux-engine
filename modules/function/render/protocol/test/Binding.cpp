#include <lux/engine/render/protocol/CommandMethod.hpp>
#include <cstdlib>

using namespace lux::render;

struct Command final
{
    static constexpr std::string_view name = "test.typed.binding";
    struct Request { unsigned value; };
    using Response = RenderResult<unsigned>;
};
struct OtherRequest { unsigned value; };

struct MemberHandler final
{
    unsigned calls = 0;
    void apply(const Command::Request& input, Command::Response& response) noexcept
    {
        ++calls;
        response = input.value + 1;
    }
    void wrong(const OtherRequest&, Command::Response&) noexcept {}
    void throwing(const Command::Request&, Command::Response&) {}
    unsigned nonVoid(const Command::Request&, Command::Response&) noexcept { return 0; }
    void byValue(const Command::Request&, Command::Response) noexcept {}
};
static_assert(SyncCommandHandlerFor<Command, MemberHandler, &MemberHandler::apply>);
static_assert(!SyncCommandHandlerFor<Command, MemberHandler, &MemberHandler::wrong>);
static_assert(!SyncCommandHandlerFor<Command, MemberHandler, &MemberHandler::throwing>);
static_assert(!SyncCommandHandlerFor<Command, MemberHandler, &MemberHandler::nonVoid>);
static_assert(!SyncCommandHandlerFor<Command, MemberHandler, &MemberHandler::byValue>);
struct InvalidResponseCommand final : OtherRequest
{
    static constexpr std::string_view name = "test.response.cannot-initialize-error";
    using Request = Command::Request;
    using Response = unsigned;
};
struct InvalidResponseHandler final
{
    void apply(const Command::Request&, unsigned&) noexcept;
};
static_assert(!SyncCommandHandlerFor<InvalidResponseCommand, InvalidResponseHandler, &InvalidResponseHandler::apply>);
static_assert(!std::is_same_v<HandlerId, CommandId>);
static_assert(sizeof(OtherRequest) == sizeof(Command::Request));

int main()
{
    MemberHandler handler;
    Command::Response response = lux::cxx::unexpected(RenderError{kProtocolContract});
    using Binding = CommandMethod<Command, &MemberHandler::apply>;
    (handler.*Binding::method)({47}, response);
    if (!response || *response != 48 || handler.calls != 1) std::abort();
    const CommandId id{17, 23};
    if (commandIdFromTag(commandTag(id)) != id) std::abort();
}
