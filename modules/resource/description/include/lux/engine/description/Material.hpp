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
        std::vector<std::array<float, 4U>> parameter_defaults;
        EAlphaMode alpha_mode{EAlphaMode::OPAQUE_SURFACE};
        bool double_sided{};
        std::vector<std::uint32_t> gbuffer_spirv;
        ShaderInfo gbuffer_info;
        std::vector<std::uint32_t> forward_spirv;
        ShaderInfo forward_info;
        std::vector<lux::asset::AssetId> texture_slot_ids;
    };

    struct MaterialInstanceDescription final
    {
        struct ParameterOverride final
        {
            std::uint32_t slot;
            std::array<float, 4> value;
        };

        struct TextureOverride final
        {
            std::uint32_t slot;
            lux::asset::AssetId texture;
        };

        lux::asset::AssetId parent_material_id;
        std::vector<ParameterOverride> parameters;
        std::vector<TextureOverride> textures;
        std::uint32_t render_state_override{};
        EAlphaMode alpha_mode{EAlphaMode::OPAQUE_SURFACE};
        bool double_sided{};
    };
} // namespace lux::rdesc
