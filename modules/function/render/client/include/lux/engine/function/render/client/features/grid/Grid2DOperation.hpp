#pragma once
// ============================================================================
//  Grid2DOperation.hpp — Grid2D 通信外观的【作者声明】(A+)
//  2D 参考网格(XY 平面,正交;着色器内缩放自适应 LOD)。onTop 选择画在
//  2D 内容之下(Transparent 段)还是之上(Overlay 段)。
//
//  Operation 面(Op 描述符/OperationIds/Proxy/createFn/registrar/factory)
//  由 engine_add_comm_ops 生成到 <lux/engine/function/render/client/genops/
//  Grid2DOperation.ops.hpp|.ops.cpp>;手写残余 = handleGrid2DSetParams
//  (Grid2DOperationHandlers.cpp)。
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    /// Comm-layer config for Grid2DPassFeature (defaults = builtins).
    struct LUX_COMM_CONFIG(
        prefix = Grid2D,
        id = lux.render.grid2d.v1,
        display = Grid2DPass,
        feature = Grid2DPassFeature,
        feature_header = lux / engine / render / renderer / features / grid / Grid2DPassFeature.hpp) Grid2DCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
    };
    static_assert(std::is_trivially_copyable_v<Grid2DCommConfig>);

    struct LUX_OP(lane = program, kind = stream, name = Grid2DSetParams, method = setParams) Grid2DSetParamsPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        float cellSize{1.f};    ///< world units per minor cell
        float majorEvery{10.f}; ///< minor cells per major line
        float linePx{1.f};      ///< line width in pixels
        std::uint32_t onTop{0}; ///< 1 = draw over the 2D content, 0 = under it
    };
    static_assert(std::is_trivially_copyable_v<Grid2DSetParamsPayload>);

} // namespace lux::render
