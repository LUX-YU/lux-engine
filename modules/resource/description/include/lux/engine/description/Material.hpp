#pragma once

#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace lux::rdesc
{
    struct MaterialDescription final
    {
        inline static constexpr std::uint32_t kMaxParams = 16U;
        inline static constexpr std::uint32_t kMaxTextures = 8U;

        std::array<std::array<float, 4U>, kMaxParams> parameter_defaults{};
        std::uint32_t parameter_count{};
        EAlphaMode alpha_mode{EAlphaMode::Opaque};
        bool double_sided{};
        std::vector<std::uint32_t> gbuffer_spirv;
        ShaderInfo gbuffer_info;
        std::vector<std::uint32_t> forward_spirv;
        ShaderInfo forward_info;
        std::array<lux::asset::AssetId, kMaxTextures> texture_slot_ids{};
    };

    struct MaterialInstanceDescription final
    {
        inline static constexpr std::uint32_t kMaxParams = MaterialDescription::kMaxParams;
        inline static constexpr std::uint32_t kMaxTextures = MaterialDescription::kMaxTextures;

        lux::asset::AssetId parent_material_id;
        std::uint32_t param_override_mask{};
        std::array<std::array<float, 4U>, kMaxParams> params{};
        std::uint32_t tex_override_mask{};
        std::array<lux::asset::AssetId, kMaxTextures> texture_slot_ids{};
        std::uint32_t render_state_override{};
        EAlphaMode alpha_mode{EAlphaMode::Opaque};
        bool double_sided{};
    };
} // namespace lux::rdesc
