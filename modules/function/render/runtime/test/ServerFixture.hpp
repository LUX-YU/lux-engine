#pragma once
#include "../../resource/test/CpuFixture.hpp"
#include <lux/engine/render/runtime/VulkanServer.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommandHandler.hpp>

using lux::render::test::CpuFixture;


inline lux::render::RenderResult<std::unique_ptr<lux::render::RenderServer>> createTestServer(
    CpuFixture& host, lux::render::RenderServerConfig config,
    lux::render::vulkan::DeviceOptions options = {.dynamic_rendering = true}
) noexcept
{
    return lux::render::vulkan::createRenderServer(std::move(config), host.runtime(), options);
}

inline lux::render::RenderResult<std::unique_ptr<lux::render::RenderServer>> createTestServer(
    lux::render::RenderServerConfig config,
    lux::render::vulkan::DeviceOptions options = {.dynamic_rendering = true}
) noexcept
{
    // Each caller stops and joins its Server before this host runtime is destroyed.
    static CpuFixture host;
    return createTestServer(host, std::move(config), options);
}
