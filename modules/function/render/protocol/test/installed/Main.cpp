#include <lux/engine/render/protocol/Protocol.hpp>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native dependency in Protocol
#endif

static_assert(__cplusplus >= 202002L);

struct InstalledQuery final
{
    static constexpr std::string_view name = "installed.Query";
    struct Request final { std::uint64_t key = 0; };
    using Response = lux::render::RenderResult<lux::render::Ack>;
};

int main()
{
    const lux::render::RequestId request{};
    const lux::render::CommandId command{7, 3};
    return !request.isNull() || lux::render::commandIdFromTag(lux::render::commandTag(command)) != command ||
        lux::render::renderProtocolErrorDescriptors().empty() ||
        !lux::render::validateCommand(lux::render::commandDescriptor<InstalledQuery>());
}
