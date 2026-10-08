#include <lux/engine/render/comm/server/RenderServer.hpp>

#include <cassert>

// Separate TU: this calls the real backend DLL, without the native pool probes.
void checkServerStartupPrefix()
{
    using namespace lux::render;
    for (unsigned attempt = 0; attempt < 4; ++attempt)
    {
        GeneralRenderServer server(
            TRenderProgramChannel<>::create(2),
            TRenderControlChannel<>::create(2),
            TRenderUploadChannel<>::create(2, 1024),
            std::make_shared<RenderChannelSync>()
        );
        ServerConfig config;
        config.capacity_request.set(capacityId("test.unknown.capacity"), CapacityValue::exact(1));
        const auto result = server.init(std::move(config));
        assert(!result && isError<err::memory::CapacityExhausted>(result.error()));
        // Device creation succeeded, but ResourceContext has not been adopted.
        // Destruction must not access pools or dependent frame/target owners.
    }
}
