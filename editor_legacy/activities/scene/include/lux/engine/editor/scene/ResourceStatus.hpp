#pragma once
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
namespace lux::editor::scene
{
    struct ResourceStatusSnapshot final
    {
        lux::scene::SceneInstanceId instance;
        lux::system::SystemInstanceId system;
        std::uint64_t revision{};
        std::vector<lux::scene::RenderAssetStatus> rows;
    };
    struct ResourceRetryRequest final
    {
        lux::scene::SceneInstanceId instance;
        lux::system::SystemInstanceId system;
        lux::scene::RenderAssetKey key;
    };
    [[nodiscard]] render::RenderResult<ResourceStatusSnapshot> captureResourceStatus(
        const lux::scene::SceneRuntime&,
        lux::scene::SceneInstanceId,
        lux::system::SystemInstanceId
    );
    [[nodiscard]] render::RenderResult<void> retryResource(lux::scene::SceneRuntime&, ResourceRetryRequest);
}
