#pragma once

#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <array>

namespace lux::render
{
    class CommandBinding;
    struct ResourceRegistration;
}

namespace lux::render::detail
{
    [[nodiscard]] std::span<const CommandBinding> meshInstanceCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> directLightingStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> materialCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> indexedTriangleMeshStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> sampledTexture2DStorageCommands() noexcept;
    [[nodiscard]] std::span<const CommandBinding> transformStorageCommands() noexcept;

    [[nodiscard]] ResourceRegistration sceneViewRegistration() noexcept;
    [[nodiscard]] CommandBinding lightingEnvironmentBinding() noexcept;
    [[nodiscard]] CommandBinding toneMappingExposureBinding() noexcept;

    [[nodiscard]] ImplementationEntry depthPyramidImplementation() noexcept;
    [[nodiscard]] std::array<ImplementationEntry, 2> lightingImplementations() noexcept;
    [[nodiscard]] std::array<ImplementationEntry, 3> toneMappingImplementations() noexcept;
    [[nodiscard]] ImplementationEntry meshVisibilityImplementation() noexcept;
    [[nodiscard]] std::array<ImplementationEntry, 2> gBufferImplementations() noexcept;
    [[nodiscard]] std::array<ImplementationEntry, 4> meshRasterImplementations() noexcept;
    [[nodiscard]] std::array<ImplementationEntry, 2> shadowImplementations() noexcept;
}
