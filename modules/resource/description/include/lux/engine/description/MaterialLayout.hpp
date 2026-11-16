#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace lux::rdesc
{
    // Stable page addressing: the buffer slot selects the physical FIF replica, then a record within it.
    struct MaterialGpuReference final
    {
        std::uint32_t buffer_index = UINT32_MAX;
        std::uint32_t record_index = 0;
    };

    static_assert(sizeof(MaterialGpuReference) == 8 && alignof(MaterialGpuReference) == 4);

    // Texture-record values shared by emitted material layouts and their target-specific consumers.
    // Placement in a record still comes from the target's reflection.
    struct MaterialTextureGpuRef final
    {
        std::uint32_t representation_index = 0;
        std::uint32_t resource_index = 0;
        std::uint32_t sampler_index = 0;
        std::uint32_t flags = 0;
    };

    static_assert(sizeof(MaterialTextureGpuRef) == 16 && alignof(MaterialTextureGpuRef) == 4);

    inline constexpr std::uint32_t kMaterialShaderAbi = 5;
    inline constexpr std::uint32_t kTextureDefaultWhite = 1u << 0;

    // Cooked Material field names shared by emission and reflected-layout consumers.
    // Offsets and stride still come only from reflection, never from these names.
    inline constexpr std::string_view kMaterialTableName = "uMats";
    inline constexpr std::string_view kMaterialParametersName = "params";
    inline constexpr std::string_view kMaterialTexturesName = "tex";
    inline constexpr std::array<std::string_view, 4>
        kMaterialTextureFields{"representation_index", "resource_index", "sampler_index", "flags"};
} // namespace lux::rdesc
