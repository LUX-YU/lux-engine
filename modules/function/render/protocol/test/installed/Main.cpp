#include <lux/engine/render/protocol/Protocol.hpp>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native dependency in Protocol
#endif

static_assert(__cplusplus >= 202002L);

struct InstalledQuery final
{
    static constexpr std::string_view name = "installed.Query";
    struct Request final { std::uint64_t key = 0; };
    using Response = lux::render::Ack;
};

int main()
{
    lux::render::RequestHeader request;
    lux::render::ReplyHeader reply;
    return !request.request.isNull() || reply.outcome != lux::render::ECommandOutcome::OUTCOME_UNKNOWN ||
        lux::render::renderProtocolErrorDescriptors().empty() ||
        !lux::render::validateCommand(lux::render::commandDescriptor<InstalledQuery>());
}
