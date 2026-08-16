#pragma once
// ============================================================================
//  SkyboxOperation.hpp — Skybox 通信外观的【作者声明】(A+)
//  两个即发即忘 Stream op(equirect / cubemap 纹理切换)。Operation 面由
//  engine_add_comm_ops 生成到 <lux/engine/function/render/client/genops/
//  SkyboxOperation.ops.hpp|.ops.cpp>;手写残余 = handleSkyboxSet*(语义)
//  与 SkyboxCreateFn(SkyboxOperationHandlers.cpp)。
//
//  custom_create=true:CommConfig→Config 不是同名字段抄写 —— createFn 里有
//  「LDR 管线时 color_input 钉到 SceneColor」的装配决策,按 §7.5 判据这类
//  非同构逻辑留手写,生成 cpp 只 extern 声明它。
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>

#include <string_view>
#include <type_traits>

namespace lux::render
{
    // =========================================================================
    //  Default shader name constants for SkyboxFeature
    // =========================================================================
    inline constexpr std::string_view kSkyboxVertShaderName         = "skybox.vert";
    inline constexpr std::string_view kSkyboxCubemapFragShaderName  = "skybox_cubemap.frag";
    inline constexpr std::string_view kSkyboxEquirectFragShaderName = "skybox_equirect.frag";

    /// Comm-layer config for SkyboxFeature.
    struct LUX_COMM_CONFIG(prefix=Skybox, id=lux.render.skybox.v1, display=Skybox,
                           custom_create=true)
    SkyboxCommConfig
    {
        ShaderHandle vertex_shader{};
        ShaderHandle cubemap_fragment{};
        ShaderHandle equirect_fragment{};
    };
    static_assert(std::is_trivially_copyable_v<SkyboxCommConfig>);

    /// Set an equirectangular (2D) texture as the skybox.
    struct LUX_OP(lane=frame, kind=stream, name=SkyboxSetEquirect, method=setEquirect)
    SkyboxSetEquirectPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        RTextureHandle texture{};
        float rotation_radians{0.0f};
        float intensity{1.0f};
    };
    static_assert(std::is_trivially_copyable_v<SkyboxSetEquirectPayload>);

    /// Set a cubemap texture as the skybox.
    struct LUX_OP(lane=frame, kind=stream, name=SkyboxSetCubemap, method=setCubemap)
    SkyboxSetCubemapPayload
    {
        RenderSceneId scene_id{};
        FeatureHandle feature{};
        RTextureHandle cube{};
        float rotation_radians{0.0f};
        float intensity{1.0f};
    };
    static_assert(std::is_trivially_copyable_v<SkyboxSetCubemapPayload>);

    struct SkyboxStatsReply final
    {
        std::uint32_t active_mode{0u};
        std::uint32_t bindless_index{0u};
        std::uint32_t pass_visits{0u};
        std::uint32_t draws{0u};
        std::uint32_t inactive_pass_visits{0u};
        std::uint32_t pipeline_bind_failures{0u};
        float intensity{0.0f};
        float rotation_radians{0.0f};
    };
    static_assert(std::is_trivially_copyable_v<SkyboxStatsReply>);

    struct LUX_OP(lane=control, kind=resource, name=SkyboxStats,
                  method=stats, reply=SkyboxStatsReply, opcode=command)
    SkyboxStatsPayload final
    {
        RenderSceneId scene_id{};
    };
    static_assert(std::is_trivially_copyable_v<SkyboxStatsPayload>);


    /// 本特性产出的 render-graph pass 名(跨 feature 引用请用常量)。
    /// Grid3D 画在天空盒之后。
    inline constexpr std::string_view kSkyboxPassName = "SkyboxPass";
} // namespace lux::render
