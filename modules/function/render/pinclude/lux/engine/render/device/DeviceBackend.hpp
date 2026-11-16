#pragma once

#include <lux/engine/render/extensions/device/DeviceSession.hpp>

namespace lux::render::detail
{
    // Non-owning, selected-backend access. No second device or capability cache.
    struct DeviceBackend final
    {
        [[nodiscard]] static RenderResult<void> initialize(
            DeviceSession&,
            const DeviceOptions&,
            const nri::CallbackInterface&,
            bool validation,
            std::uint32_t slots
        ) noexcept;

        [[nodiscard]] static RenderResult<void> initializeD3D12(
            DeviceSession&, const DeviceOptions&, const nri::CallbackInterface&, bool, std::uint32_t
        ) noexcept;
        [[nodiscard]] static RenderResult<void> validateTextureD3D12(const DeviceSession&, const nri::TextureDesc&) noexcept;
    };
} // namespace lux::render::detail
