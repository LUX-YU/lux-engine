#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/render/client/features/GpuDrivenMeshExtFlags.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>
#include <string_view>

namespace lux::render
{
    struct FeatureFactory;

    inline constexpr uint32_t kForwardMeshCommConfigVersion = 3u; // +graph_fragment
    inline constexpr uint32_t kForwardMeshDescriptorLayoutVersion = 1u;

    // =========================================================================
    //  Default shader ASSET names for ForwardMeshFeature — what a client looks
    //  up in the asset system before submitting the SPIR-V through CompileShader
    //  (that op carries bytes, not names). A client that just wants the engine's
    //  own shader does not need these at all: leave the config field empty and
    //  the feature backfills it via createBuiltinShaderModule(EBuiltinShader::FORWARD_*).
    //
    //  ⚠️ Unlike EBuiltinShader, these strings have NO build-time binding —
    //  nothing fails if an asset is renamed out from under them. That is exactly
    //  how kForwardVertShaderName came to name "forward_mesh.vert" for a while
    //  after the vertex-pool rewrite renamed the asset to forward_mesh_vp.vert.
    //  Prefer EBuiltinShader when a typed handle will do.
    // =========================================================================
    inline constexpr std::string_view kForwardCullShaderName = "mesh_cull_unified.comp";
    inline constexpr std::string_view kForwardCompactShaderName = "mdc_compact.comp";
    inline constexpr std::string_view kForwardVertShaderName = "forward_mesh_vp.vert";
    inline constexpr std::string_view kForwardUnlitFragShaderName = "fr_unlit.frag";

    /// Shadow-technique VARIANT STEMS — these two are packed per technique, so
    /// the asset name is the stem plus a suffix: ".pcf" or ".evsm"
    /// (e.g. "fr_pbr.frag.pcf"). The stem alone resolves to nothing.
    /// Unlit has no shadow path and therefore no variants.
    inline constexpr std::string_view kForwardPbrFragShaderName = "fr_pbr.frag";
    inline constexpr std::string_view kForwardStylizedFragShaderName = "fr_stylized.frag";

    /// Comm-layer config for ForwardMeshFeature.
    /// Client fills shader indices from CompileShader replies.
    struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(
        prefix = ForwardMesh,
        id = lux.render.forward_mesh.v1,
        display = ForwardMesh,
        requires = lux.render.mesh_stack.v1,
        custom_create = true) ForwardMeshCommConfig
    {
        ShaderHandle forward_cull_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle forward_compact_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle forward_vert_shader LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle unlit_fragment LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle pbr_fragment LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        ShaderHandle stylized_fragment LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        // Optional: node-graph (Graph family) forward frag. No builtin fallback —
        // null => Graph family gets no forward pipeline. Supplied via compileShader.
        // 编辑器的活材质预览已改走逐材质 forward PSO(GpuResourceCache::
        // ensureGraphMaterial 上传时自带双份 frag,R1),本 override 现仅
        // shadergen 测试路径(graph_forward_render_test)使用。
        ShaderHandle graph_fragment LUX_TYPE_MEMBER(skip_static = true) LUX_NO_MEMBER(){};
        uint32_t comm_config_version{kForwardMeshCommConfigVersion};
        uint32_t descriptor_layout_version{kForwardMeshDescriptorLayoutVersion};
        GpuDrivenMeshExtFlags extension_flags{};
    };
    static_assert(std::is_trivially_copyable_v<ForwardMeshCommConfig>);

    /// ForwardMesh feature factory — registered at runtime via RegisterFeatureType.

} // namespace lux::render
