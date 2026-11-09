#pragma once

#include <lux/engine/EngineContext.hpp>
#include <lux/engine/assets/AssetRetentionPolicy.hpp>
#include <lux/engine/core/execution/ExecutionConfig.hpp>
#include <lux/engine/resource/asset/AssetCodec.hpp>

#if defined(LUX_ENGINE_WITH_RENDER)
#include <lux/engine/render/runtime/RenderServer.hpp>
#endif

namespace lux::engine
{
    // Creates an empty, live VFS and its repository. Mounts remain the registering caller's responsibility.
    // No Scene or application window is created here. Success publishes the process runtime instances.
#if defined(LUX_ENGINE_WITH_RENDER)
    [[nodiscard]] InitResult init(
        execution::ExecutionRuntimeConfig execution,
        asset::AssetDecodeLimits asset_limits,
        lux::render::RenderServerConfig rendering,
        lux::render::DeviceOptions device,
        std::unique_ptr<assets::AssetRetentionPolicy> retention = {},
        std::vector<std::unique_ptr<asset::AssetCodec>> additional_codecs = {},
        std::function<lux::render::RenderResult<void>(lux::render::RenderServer&)> register_render = {}
    ) noexcept;
#else
    [[nodiscard]] InitResult init(
        execution::ExecutionRuntimeConfig execution,
        asset::AssetDecodeLimits asset_limits,
        std::unique_ptr<assets::AssetRetentionPolicy> retention = {},
        std::vector<std::unique_ptr<asset::AssetCodec>> additional_codecs = {}
    ) noexcept;
#endif
} // namespace lux::engine
