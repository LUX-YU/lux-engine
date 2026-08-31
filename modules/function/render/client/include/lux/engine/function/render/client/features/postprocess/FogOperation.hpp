#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct LUX_COMM_CONFIG(
        prefix = Fog,
        id = lux.render.fog.v1,
        display = Fog,
        requires = lux.render.linear_depth.v1,
        feature = FogFeature,
        feature_header = lux / engine / render / renderer / features / postprocess / FogFeature.hpp) FogCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
    };
    static_assert(std::is_trivially_copyable_v<FogCommConfig>);

    struct LUX_OP(lane = program, kind = stream, name = FogSetParams, method = setParams) FogSetParamsPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        float color[3]{0.55f, 0.62f, 0.70f};
        float density{0.0002f};
        float start_distance{0.0f};
        float reference_height{0.0f};
        float height_falloff{0.01f};
        float maximum_opacity{0.98f};
        std::uint32_t enabled{0u};
    };
    static_assert(std::is_trivially_copyable_v<FogSetParamsPayload>);
} // namespace lux::render
