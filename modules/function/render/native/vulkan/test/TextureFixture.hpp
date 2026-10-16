#pragma once
#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/TextureUpload.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <algorithm>
#include "../../../resource/test/CpuFixture.hpp"
#include <thread>
using namespace foundation_test;
namespace rdesc = lux::rdesc;
using Clock = std::chrono::steady_clock;

using lux::render::test::CpuFixture;

template<class Work> static RenderResult<bool> advanceAfterCpu(CpuFixture& host, Work& work) noexcept
{
    (void)host.drain();
    return work.advance();
}

static rdesc::TextureInfo information(unsigned dimension, unsigned mips, bool bc)
{
    rdesc::TextureInfo info{};
    info.width = info.height = dimension;
    info.channel = 4;
    info.mip_count = mips;
    info.pixel_format = bc ? rdesc::ETexturePixelFormat::BC1_SRGB : rdesc::ETexturePixelFormat::RGBA8_UNORM;
    info.color_space = bc ? rdesc::ETextureColorSpace::SRGB : rdesc::ETextureColorSpace::DATA;
    info.flags = bc ? rdesc::toUnderlying(rdesc::ETextureAssetFlags::COMPRESSED) : 0;
    std::uint64_t offset = 0;
    for (unsigned i = 0; i < mips; ++i)
    {
        const auto extent = std::max(1u, dimension >> i);
        const std::uint64_t bytes = bc ? std::uint64_t((extent + 3) / 4) * ((extent + 3) / 4) * 8 :
            std::uint64_t(extent) * extent * 4;
        info.mip_ranges[i] = {offset, bytes, extent, extent};
        offset += bytes;
    }
    return info;
}

static lux::render::vulkan::TextureStorage* textures(DeviceSession& session) noexcept
{
    auto& registry = session.resources();
    auto id = registry.find(registry.root(), "default", lux::render::TextureStorage::description);
    return id ? static_cast<lux::render::vulkan::TextureStorage*>(registry.native(*id)) : nullptr;
}

static RenderResult<void> configureTextures(DeviceSession& session, std::uint32_t capacity, std::uint64_t bytes) noexcept
{
    auto& registry = session.resources();
    const TextureStorageConfig config{capacity, bytes};
    if (!registry.configuration(lux::render::TextureStorage::description.type))
        if (auto configured = registry.configureType(lux::render::TextureStorage::description,
            std::make_shared<const TextureStorageConfig>(config)); !configured) return configured;
    auto id = registry.getOrCreate(registry.root(), "default", lux::render::TextureStorage::description, &config);
    if (!id) return lux::cxx::unexpected(id.error());
    return {};
}
