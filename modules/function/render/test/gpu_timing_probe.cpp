#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

int main()
{
    using namespace lux::render;

    lux::rendertest::DeviceRenderFixture fixture(
        32u,
        32u,
        "gpu_timing_probe"
    );
    if (!fixture.ok())
    {
        std::puts("SKIP: Vulkan device unavailable");
        return 0;
    }

    const auto scene = fixture.makeSceneWithView(
        "GpuTimingProbe",
        "GpuTimingProbeView"
    );
    const auto registered = fixture.awaitControl(
        fixture.control().registerFeatureType(kCanvas2DFeatureFactory)
    );
    const auto attached = fixture.awaitControl(
        fixture.control().addFeature(
            scene.scene_id,
            registered.feature_type_id,
            Canvas2DCommConfig{}
        )
    );
    if (!attached.feature.isValid())
        return 1;

    fixture.flush(8);

    std::string storage(64u * 1024u, '\0');
    const auto reply = fixture.awaitControl(
        fixture.control().queryGpuTiming(
            scene.scene_id,
            storage.data(),
            storage.size()
        )
    );
    const std::string_view json{
        storage.data(),
        std::min<std::size_t>(reply.written, storage.size())
    };
    if (reply.status != 0u
        || json.find("\"version\":2") == std::string_view::npos
        || json.find("\"views\":[{") == std::string_view::npos
        || json.find("\"available\":") == std::string_view::npos)
    {
        std::fprintf(
            stderr,
            "invalid GPU timing snapshot: %.*s\n",
            static_cast<int>(json.size()),
            json.data()
        );
        return 2;
    }

    if (json.find("\"available\":true") != std::string_view::npos
        && json.find("Canvas2D") == std::string_view::npos)
    {
        std::fprintf(
            stderr,
            "timestamp-capable device returned no Canvas2D pass: %.*s\n",
            static_cast<int>(json.size()),
            json.data()
        );
        return 3;
    }
    return 0;
}
