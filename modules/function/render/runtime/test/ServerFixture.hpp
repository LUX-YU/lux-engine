#pragma once
#include "../../resource/test/CpuFixture.hpp"
#include <lux/engine/render/runtime/VulkanServer.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureCommandHandler.hpp>

using lux::render::test::CpuFixture;

#ifdef LUX_RUNTIME_CPU_TEST_SEAM
namespace lux::render::runtime_cpu_test { extern CpuSchedule controlled_cpu; }
#endif

inline lux::render::RenderResult<std::unique_ptr<lux::render::RenderServer>> createTestServer(
    CpuFixture& host, lux::render::RenderServerConfig config,
    lux::render::vulkan::DeviceOptions options = {.dynamic_rendering = true}
) noexcept
{
#ifdef LUX_RUNTIME_CPU_TEST_SEAM
    lux::render::runtime_cpu_test::controlled_cpu = host.cpu();
#endif
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
