#pragma once
#include "../../resource/test/CpuFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>

using lux::render::test::CpuFixture;

inline lux::render::RenderServerConfig
withCpu(CpuFixture& host, lux::render::RenderServerConfig config)
{
    config.cpu = host.cpu();
    return config;
}

inline lux::render::RenderResult<std::unique_ptr<lux::render::RenderServer>>
createTestServer(lux::render::RenderServerConfig config)
{
    // Tests stop and join each server before reusing this actual host CpuScheduler.
    static CpuFixture host;
    return lux::render::RenderServer::create(withCpu(host, std::move(config)));
}
