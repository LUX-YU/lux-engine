#include "Support.hpp"

#if defined(LUX_REJECT_VULKAN)
#include <vulkan/vulkan.h>
#elif defined(LUX_REJECT_RUNTIME)
#include <lux/engine/render/RenderRuntime.hpp>
#elif defined(LUX_REJECT_SCENE)
#include <lux/engine/scene/SceneSystem.hpp>
#endif

int main()
{
    transport_test::Fixture fixture;
    auto packet = fixture.transport->makeProgramPacket();
#if defined(LUX_REJECT_LANE)
    transport_test::must(packet.request(fixture.ping, transport_test::Ping{}));
#elif defined(LUX_REJECT_KIND)
    transport_test::must(packet.write(fixture.bulk, transport_test::Bulk{}));
#elif defined(LUX_REJECT_PAYLOAD)
    transport_test::must(packet.write(fixture.value, transport_test::Bulk{}));
#else
    transport_test::must(packet.write(fixture.value, transport_test::Value{1}));
#endif
    return 0;
}
