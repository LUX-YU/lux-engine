#pragma once
/**
 * @file MaterialGpuTypes.hpp
 * @brief GPU-compatible material structs for the 4-family material system.
 *
 * Four family GPU structs, one per ELightingTechnique:
 *   UnlitFamilyGPU       (96B)  — Unlit materials
 *   LegacyLitFamilyGPU   (256B) — LegacyLit: diffuse+specular closures + layers
 *   PbrFamilyGPU          (256B) — PBR metallic-roughness + layers
 *   StylizedFamilyGPU     (192B) — Toon / ramp-based
 *
 * Each struct is self-contained: no intermediate "legacy typed GPU struct"
 * step.  Convert directly from rdesc::Material payloads.
 */

#include <lux/engine/render/core/LayoutTypes.hpp>
#include <lux/engine/render/core/MaterialFamily.hpp>   // ELightingTechnique (+ MaterialEnums)
#include <Eigen/Core>                                  // GPU-struct vector fields (was via the deleted Material.hpp)

#include <cstring>

namespace lux::render
{
    // ===== Material Common Flags =====
    enum : uint32_t {
        MATF_DOUBLE_SIDED    = 1u << 0,
        MATF_CAST_SHADOWS    = 1u << 1,
        MATF_RECEIVE_SHADOWS = 1u << 2,
        MATF_SMITH_MASKING   = 1u << 3,
    };

    // ===== Texture Reference (bindless index), aligned to 16B =====
    struct alignas(16) TextureRefGPU {
        uint32_t bindless{ 0 };
        uint32_t _pad[3]{};
    };
    static_assert(sizeof(TextureRefGPU) % 16 == 0);

    // ===== Eigen → GPU vector conversion helpers =====
    namespace detail {
        inline aligned16vec3 vec3ToGPU(const Eigen::Vector3f& v) { return { v.x(), v.y(), v.z() }; }
        inline aligned16vec4 vec4ToGPU(const Eigen::Vector4f& v) { return { v.x(), v.y(), v.z(), v.w() }; }
        inline aligned16vec4 vec3ToVec4(const Eigen::Vector3f& v, float w = 0.0f) {
            return { v.x(), v.y(), v.z(), w };
        }
        // (bindTex(optional<rdesc::TextureBinding>) retired in W5a with the builtin
        //  closure-material converters below.)
    }

    // =====================================================================
    //  Family 0: Unlit (96B)
    // =====================================================================
    struct alignas(16) UnlitFamilyGPU
    {
        uint32_t      shading_model_id;   // EShadingModel::UNLIT = 0
        uint32_t      feature_mask;       // ShaderFeatureMask
        uint32_t      tex_mask;           // bit0: base_color, bit1: emissive
        uint32_t      flags;              // MATF_*
        uint32_t      _header_pad[4]{};   // pad header to 32B

        aligned16vec4 base_color;         // rgba
        aligned16vec4 emissive;           // rgb, 0

        TextureRefGPU tex[2];             // [base_color, emissive]
    };
    static_assert(sizeof(UnlitFamilyGPU) == 96, "UnlitFamilyGPU must be 96 bytes");

    // =====================================================================
    //  Family 1: LegacyLit (256B)
    // =====================================================================
    struct alignas(16) LegacyLitFamilyGPU
    {
        uint32_t      shading_model_id;   // 100 + diffuse*10 + specular
        uint32_t      feature_mask;
        uint32_t      tex_mask;           // bit0: albedo, bit1: normal, bit2: emissive,
                                          // bit3: specular_color, bit4: specular_param
        uint32_t      flags;
        uint32_t      _header_pad[4]{};   // pad header to 32B

        // --- param area (128B = 8 × vec4) ---
        aligned16vec4 albedo;             // rgb, 0
        aligned16vec4 emissive;           // rgb, 0
        aligned16vec4 specular_color;     // rgb, 0   (Phong/BlinnPhong/CookTorrance)
        aligned16vec4 specular_params;    // x=shininess/glossiness/roughness,
                                          // y=metalness, z=ior, w=distribution(as uint bits)
        aligned16vec4 diffuse_params;     // x=sigma_deg(OrenNayar), y=k(Minnaert), z=0, w=0
        aligned16vec4 layer_params;       // x=fresnel_strength, y=clearcoat_factor,
                                          // z=clearcoat_roughness, w=sheen_roughness
        aligned16vec4 layer_colors;       // xyz=sheen_color, w=0
        aligned16vec4 _reserved;

        // --- texture area (96B = 6 slots) ---
        TextureRefGPU tex[6];             // [albedo, normal, emissive, spec_color/param,
                                          //  fresnel_mask, clearcoat_normal]
    };
    static_assert(sizeof(LegacyLitFamilyGPU) == 256, "LegacyLitFamilyGPU must be 256 bytes");

    // =====================================================================
    //  Family 2: PBR Metallic-Roughness (256B)
    // =====================================================================
    struct alignas(16) PbrFamilyGPU
    {
        uint32_t      shading_model_id;   // EShadingModel::PbrMetallicRoughness = 200
        uint32_t      feature_mask;
        uint32_t      tex_mask;           // bit0: base_color, bit1: metallic_roughness,
                                          // bit2: normal, bit3: occlusion, bit4: emissive
        uint32_t      flags;
        uint32_t      _header_pad[4]{};   // pad header to 32B

        // --- param area (128B = 8 × vec4) ---
        aligned16vec4 base_color;         // rgba
        aligned16vec4 pbr_params;         // metallic, roughness, normal_scale, ao_strength
        aligned16vec4 emissive;           // rgb, 0
        aligned16vec4 alpha_params;       // alpha_mode(as uint bits), alpha_cutoff, ior, 0
        aligned16vec4 layer_params;       // fresnel_strength, clearcoat_factor,
                                          // clearcoat_roughness, sheen_roughness
        aligned16vec4 layer_colors;       // xyz=sheen_color, w=0
        aligned16vec4 _reserved0;
        aligned16vec4 _reserved1;

        // --- texture area (96B = 6 slots) ---
        TextureRefGPU tex[6];             // [base_color, metallic_roughness, normal,
                                          //  occlusion, emissive, clearcoat_normal]
    };
    static_assert(sizeof(PbrFamilyGPU) == 256, "PbrFamilyGPU must be 256 bytes");

    // =====================================================================
    //  Family 3: Stylized (192B)
    // =====================================================================
    struct alignas(16) StylizedFamilyGPU
    {
        uint32_t      shading_model_id;   // EShadingModel::STYLIZED = 300
        uint32_t      feature_mask;
        uint32_t      tex_mask;           // bit0: base_color, bit1: ramp,
                                          // bit2: normal, bit3: emissive
        uint32_t      flags;
        uint32_t      _header_pad[4]{};   // pad header to 32B

        // --- param area (96B = 6 × vec4) ---
        aligned16vec4 base_color;         // rgb, 0
        aligned16vec4 toon_params;        // diffuse_steps, specular_steps,
                                          // edge_threshold, sigma_deg(-1 if none)
        aligned16vec4 emissive;           // rgb, 0
        aligned16vec4 _reserved0;
        aligned16vec4 _reserved1;
        aligned16vec4 _reserved2;

        // --- texture area (64B = 4 slots) ---
        TextureRefGPU tex[4];             // [base_color, ramp, normal, emissive]
    };
    static_assert(sizeof(StylizedFamilyGPU) == 192, "StylizedFamilyGPU must be 192 bytes");

    // =====================================================================
    //  Family 4: Graph (416B) — node-graph materials
    //
    //  Generic, graph-agnostic per-material blob: a fixed array of param lanes
    //  (param slot i -> params[i], swizzled to the slot's declared arity) plus a
    //  texture table. The graph-generated fragment shader reads BOTH its params
    //  and its textures from here (set 4, binding 4), indexed by the flat
    //  vMatIndex like the builtin families.
    //
    //  ABI MIRROR: this MUST match the inlined `GraphMaterialGPU` struct emitted
    //  by engine/material_graph_glsl Emitter.cpp graphFamily() byte-for-byte:
    //    32B header, 16 vec4 param lanes, 8 texture refs.
    // =====================================================================
    struct alignas(16) GraphFamilyGPU
    {
        uint32_t      shading_model_id;   // EShadingModel::GRAPH = 400
        uint32_t      feature_mask;       // unused (graph bakes its own shading)
        uint32_t      tex_mask;           // bit i set if tex slot i bound
        uint32_t      flags;              // MATF_*
        uint32_t      _header_pad[4]{};   // pad header to 32B

        aligned16vec4 params[16];         // generic param lanes (256B)
        TextureRefGPU tex[8];             // graph texture table (128B)
    };
    static_assert(sizeof(GraphFamilyGPU) == 416, "GraphFamilyGPU must be 416 bytes (matches glsl GraphMaterialGPU)");

    // =====================================================================
    //  Conversion: rdesc payloads → family GPU structs — RETIRED in W5a.
    //
    //  buildMatFlags + the four convertMaterial(rdesc::{Unlit,LegacyLit,Pbr,
    //  Stylized}Material) overloads + applyLayers(rdesc::Material) were the builtin
    //  closure-material -> GPU converters, fed by MaterialResources::submit/modify
    //  (also retired). The four builtin family GPU structs above are kept (they are
    //  rdesc::Material-free PODs) but are now unfed; graph materials pack their own
    //  blob via MaterialResources::packGraphGpu. A later cleanup can drop the dead
    //  builtin structs + SSBOs + family frags entirely (Graph = the sole family).
    // =====================================================================

    // =====================================================================
    //  Family GPU type trait: ELightingTechnique → GPU struct type
    // =====================================================================
    template<ELightingTechnique> struct family_gpu_type;
    template<> struct family_gpu_type<ELightingTechnique::Unlit>      { using type = UnlitFamilyGPU; };
    template<> struct family_gpu_type<ELightingTechnique::LegacyLit> { using type = LegacyLitFamilyGPU; };
    template<> struct family_gpu_type<ELightingTechnique::PbrMetallicRoughness>     { using type = PbrFamilyGPU; };
    template<> struct family_gpu_type<ELightingTechnique::Stylized>   { using type = StylizedFamilyGPU; };
    template<> struct family_gpu_type<ELightingTechnique::Graph>      { using type = GraphFamilyGPU; };

    template<ELightingTechnique F>
    using family_gpu_t = typename family_gpu_type<F>::type;

} // namespace lux::render