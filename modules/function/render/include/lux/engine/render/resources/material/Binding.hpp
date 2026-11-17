#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/resources/material/Data.hpp>

namespace lux::render
{
    struct MaterialBinding final
    {
        MaterialId material;
        rdesc::ShaderBlock layout;               // Complete reflected record ABI; image slots need no Shader copy.
        std::uint32_t texture_slot = UINT32_MAX; // UINT32_MAX selects the parameter Buffer; otherwise one Image.
        bool operator==(const MaterialBinding&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<MaterialBinding> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MaterialBinding&) noexcept;
    };
} // namespace lux::render
