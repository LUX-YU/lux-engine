#pragma once

#include <lux/engine/render/core/RenderResourceHandle.hpp>

#include <array>
#include <cstdint>
#include <variant>

namespace lux::render
{
    enum class ESkyboxCmdId : uint32_t
    {
        SetEquirectHandle = 0,
        SetCubemapHandles,
        Count
    };

    struct SetEquirectHandleCmd
    {
        static constexpr ESkyboxCmdId kCmdId = ESkyboxCmdId::SetEquirectHandle;
        RTextureHandle texture{};
    };

    struct SetCubemapHandlesCmd
    {
        static constexpr ESkyboxCmdId kCmdId = ESkyboxCmdId::SetCubemapHandles;
        RTextureHandle cube{};
    };

    using SkyboxSyncCmd = std::variant<SetEquirectHandleCmd, SetCubemapHandlesCmd>;
} // namespace lux::render
