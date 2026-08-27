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

    inline constexpr uint32_t kDeferredGBufferCommConfigVersion =
        5u; // 5:删 finalize_compute_shader 死字段(finalize 路径早已退役);4:ext-flag 位重排(LocalReadScope 1<<1 → 1<<2)
    inline constexpr uint32_t kDeferredGBufferDescriptorLayoutVersion = 1u;
    // Extension flags now come from the shared EGpuDrivenMeshExt bit space
    // (GpuDrivenMeshExtFlags.hpp) rather than a per-feature set of loose uint32
    // constants. This feature accepts HZB and LocalReadScope.
    //
    // The version number above records why that matters: LocalReadScope used to
    // be 1<<1, the same bit a never-consumed Bindless flag claimed, and pulling
    // them apart cost a comm-config version bump (3 → 4). One declaration of the
    // bit space is what stops the next collision — the compiler now sees every
    // bit at the point a new one is added.
    inline constexpr GpuDrivenMeshExtFlags kDeferredGBufferKnownExtFlags =
        GpuDrivenMeshExtFlags{EGpuDrivenMeshExt::HZB} | GpuDrivenMeshExtFlags{EGpuDrivenMeshExt::LocalReadScope};

    // =========================================================================
    //  Default shader ASSET names for DeferredGBufferFeature — what a client
    //  looks up in the asset system before submitting the SPIR-V through
    //  CompileShader (that op carries bytes, not names). A client that just
    //  wants the engine's own shader does not need these: leave the config
    //  field empty and the feature backfills it via
    //  createBuiltinShaderModule(EBuiltinShader::GBUFFER_*).
    //
    //  ⚠️ Unlike EBuiltinShader, these strings have NO build-time binding —
    //  nothing fails if an asset is renamed out from under them. That is exactly
    //  how kGBufferVertShaderName came to name "gbuffer.vert" for a while after
    //  the vertex-pool rewrite renamed the asset to gbuffer_vp.vert.
    //  Prefer EBuiltinShader when a typed handle will do.
    // =========================================================================
    inline constexpr std::string_view kGBufferVertShaderName = "gbuffer_vp.vert";
    inline constexpr std::string_view kGBufferUnlitFragShaderName = "gbuffer_unlit.frag";
    inline constexpr std::string_view kGBufferPbrFragShaderName = "gbuffer_pbr.frag";
    inline constexpr std::string_view kGBufferStylizedFragShaderName = "gbuffer_stylized.frag";
    inline constexpr std::string_view kGBufferCullShaderName = "mesh_cull_unified.comp";
    inline constexpr std::string_view kGBufferCompactShaderName = "mdc_compact.comp";

    /// Comm-layer config for DeferredGBufferFeature.
    /// Client fills shader indices from CompileShader replies.
    struct LUX_COMM_CONFIG(
        prefix = DeferredGBuffer,
        id = lux.render.deferred_gbuffer.v1,
        display = DeferredGBuffer,
        requires = lux.render.mesh_stack.v1,
        custom_create = true) DeferredGBufferCommConfig
    {
        ShaderHandle gbuffer_vertex_shader{};
        ShaderHandle gbuffer_unlit_fragment_shader{};
        ShaderHandle gbuffer_pbr_fragment_shader{};
        ShaderHandle gbuffer_stylized_fragment_shader{};
        // Optional: node-graph (Graph family) gbuffer frag. No builtin fallback —
        // when null, the Graph family simply gets no pipeline (graph materials,
        // if any, won't draw). Supplied via the override conduit (compileShader).
        ShaderHandle gbuffer_graph_fragment_shader{};
        ShaderHandle cull_compute_shader{};
        ShaderHandle compact_compute_shader{};
        uint32_t comm_config_version{kDeferredGBufferCommConfigVersion};
        uint32_t descriptor_layout_version{kDeferredGBufferDescriptorLayoutVersion};
        GpuDrivenMeshExtFlags extension_flags{};
    };
    static_assert(std::is_trivially_copyable_v<DeferredGBufferCommConfig>);

    /// DeferredGBuffer feature factory — registered at runtime via RegisterFeatureType.

    /// 本特性产出的 render-graph pass 名。**跨 feature 引用请用这个常量** ——
    /// DeferredLighting 要排在它之后。裸字面量的问题不是难看,是改名方在编译期
    /// 毫无感觉:图编译器按名字连边,连不上就只是少一条排序约束。
    inline constexpr std::string_view kDeferredGBufferDrawPassName = "DeferredGBufferDraw";
} // namespace lux::render
