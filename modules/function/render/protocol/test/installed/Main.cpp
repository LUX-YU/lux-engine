#include <lux/engine/render/protocol/Protocol.hpp>

#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native dependency in Protocol
#endif

static_assert(__cplusplus >= 202002L);

int main()
{
    lux::render::RequestHeader request;
    lux::render::ReplyHeader reply;
    return !request.request.isNull() || reply.outcome != lux::render::EOperationOutcome::OUTCOME_UNKNOWN ||
        lux::render::renderProtocolErrorDescriptors().empty();
}
