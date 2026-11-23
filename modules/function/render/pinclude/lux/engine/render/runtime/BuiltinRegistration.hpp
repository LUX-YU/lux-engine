#pragma once

#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    class RenderServer;
    class CommandBinding;
}

namespace lux::render::detail
{
    [[nodiscard]] std::span<const CommandBinding> rasterMeshInstanceStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> directLightingStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> rasterMaterialStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> indexedTriangleMeshStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> sampledTexture2DStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> transformStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> viewParameterStorageCommands() noexcept;

    [[nodiscard]] CommandBinding lightingEnvironmentBinding() noexcept;
    [[nodiscard]] CommandBinding toneMappingExposureBinding() noexcept;

    [[nodiscard]] RenderResult<void> registerDepthPyramid(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerLighting(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerMeshVisibility(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerGBuffer(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerMeshRaster(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerShadow(RenderServer&) noexcept;
    [[nodiscard]] RenderResult<void> registerToneMapping(RenderServer&) noexcept;
}
