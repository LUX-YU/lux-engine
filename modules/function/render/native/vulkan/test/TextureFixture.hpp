#pragma once
#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <algorithm>
#include "../../../resource/test/CpuFixture.hpp"
#include <thread>
using namespace foundation_test;
namespace rdesc = lux::rdesc;
using Clock = std::chrono::steady_clock;

using lux::render::test::CpuFixture;

// Test observation only. Native CPU events and queue callbacks drive the work; no per-work polling driver.
struct WorkCompletion final
{
    bool done = false;
    RenderResult<void> result;

    void start(GpuWork& work, CpuFixture& host) noexcept
    {
        CHECK(work.prepareCompletion());
        work.start(host.post(), this, [](void* user, RenderResult<void> value) noexcept {
            auto& completion = *static_cast<WorkCompletion*>(user);
            CHECK(!completion.done);
            completion.result = std::move(value);
            completion.done = true;
        });
    }
};

static void pump(CpuFixture& host, DeviceSession& session)
{
    (void)host.drain();
    CHECK(session.collect());
    session.dispatchCompletions();
}

static void finish(WorkCompletion& completion, CpuFixture& host, DeviceSession& session)
{
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (!completion.done)
    {
        pump(host, session);
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
}

static void complete(GpuWork& work, CpuFixture& host, DeviceSession& session)
{
    CHECK(!work.publish());
    WorkCompletion completion;
    completion.start(work, host);
    finish(completion, host, session);
    CHECK(completion.result);
    CHECK(work.publish());
    CHECK(!work.publish());
}

static RenderPayload payload(const std::shared_ptr<const std::vector<std::byte>>& owner)
{
    return take(RenderPayload::fromVector(owner, 0, owner->size()));
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
    auto id = session.globalStorages().find(registry.typeId(kTextureStorageType), "default");
    return static_cast<lux::render::vulkan::TextureStorage*>(registry.native(id));
}

static RenderResult<void> configureTextures(DeviceSession& session, std::uint32_t capacity, std::uint64_t bytes) noexcept
{
    auto& registry = session.resources();
    const TextureStorageConfig config{capacity, bytes};
    if (!registry.factory(lux::render::TextureStorage::description.type))
        if (auto installed = registry.installFactory(textureStorageFactory(), config); !installed)
            return lux::cxx::unexpected(installed.error());
    auto id = session.globalStorages().prepare(registry.typeId(kTextureStorageType), "default",
        *ConfigValue::make(config));
    if (!id) return lux::cxx::unexpected(id.error());
    return {};
}
