#pragma once
// ============================================================================
//  Grid3DOperation.hpp — Grid3D 通信外观的【作者声明】(A+ 打样件)
//
//  本头只剩纯声明:创建配置 + op 载荷 + luxop 注解。其余整个 Operation 面
//  (op 描述符 / CommandTraits / OperationIds / 收 Payload 的 Proxy /
//  registrar / descriptor / factory / 同名字段 createFn)由 engine_add_comm_ops
//  生成到 <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp|.ops.cpp>。
//  消费 Proxy / factory 的一侧 include 生成头(它转包含本头);只碰载荷的
//  一侧 include 本头即可。手写残余 = handleGrid3DSetParams 语义函数
//  (Grid3DOperationHandlers.cpp)。
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>

#include <type_traits>
#include <string_view>

namespace lux::render
{
    // =========================================================================
    //  Default shader name constants for Grid3DPassFeature
    // =========================================================================
    inline constexpr std::string_view kGridVertShaderName = "grid.vert";
    inline constexpr std::string_view kGridFragShaderName = "grid.frag";

    /// Comm-layer config for Grid3DPassFeature.
    /// The client fills this with shader indices returned by CompileShader.
    /// The generated create_fn copies same-named fields into Feature::Config.
    struct LUX_COMM_CONFIG(
        prefix = Grid3D,
        id = lux.render.grid3d.v1,
        display = Grid3DPass,
        feature = Grid3DPassFeature,
        feature_header = lux / engine / render / renderer / features / grid / Grid3DPassFeature.hpp) Grid3DCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle fragment_shader{};
    };
    static_assert(std::is_trivially_copyable_v<Grid3DCommConfig>);

    /// Stream op (fire-and-forget, no reply); the handler's semantics live in
    /// Grid3DOperationHandlers.cpp (handleGrid3DSetParams).
    struct LUX_OP(lane = frame, kind = stream, name = Grid3DSetParams, method = setParams) Grid3DSetParamsPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        float planeY{0.f};
        float cellSize{1.f};
        float linePx{1.1f};
        float fadeDist{50.f};
        float holeRatio{0.45f};
        std::uint32_t onTop{0}; ///< 1 = X-ray (over geometry), 0 = depth-tested
    };
    static_assert(std::is_trivially_copyable_v<Grid3DSetParamsPayload>);

} // namespace lux::render
