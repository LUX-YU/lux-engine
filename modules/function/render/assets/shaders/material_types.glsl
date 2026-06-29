#ifndef MATERIAL_TYPES_GLSL
#define MATERIAL_TYPES_GLSL
// =========================================================================
//  material_types.glsl — GPU material struct type definitions
//
//  Shared between forward fragment shaders and deferred gbuffer.frag.
//  Contains ONLY struct types — no layout bindings.
//  Each shader declares its own SSBO bindings using these types.
// =========================================================================

struct TextureRefGPU {
    uint bindless; uint _pad0; uint _pad1; uint _pad2;
};

// Unlit family (EShadingModel::UNLIT = 0)
struct UnlitMaterialGPU {
    uint  shading_model_id;
    uint  feature_mask;
    uint  tex_mask;
    uint  flags;
    uint  _header_pad[4];
    vec4  base_color;
    vec4  emissive;
    TextureRefGPU tex[2];
};

// LegacyLit family (100 + diffuse*10 + specular)
struct LegacyLitMaterialGPU {
    uint  shading_model_id;
    uint  feature_mask;
    uint  tex_mask;
    uint  flags;
    uint  _header_pad[4];
    vec4  albedo;
    vec4  emissive;
    vec4  specular_color;
    vec4  specular_params;
    vec4  diffuse_params;
    vec4  layer_params;
    vec4  layer_colors;
    vec4  _reserved;
    TextureRefGPU tex[6];
};

// PBR family (EShadingModel::PbrMetallicRoughness = 200)
struct PbrMaterialGPU {
    uint  shading_model_id;
    uint  feature_mask;
    uint  tex_mask;
    uint  flags;
    uint  _header_pad[4];
    vec4  base_color;
    vec4  pbr_params;
    vec4  emissive;
    vec4  alpha_params;
    vec4  layer_params;
    vec4  layer_colors;
    vec4  _reserved0;
    vec4  _reserved1;
    TextureRefGPU tex[6];
};

// Stylized family (EShadingModel::STYLIZED = 300)
struct StylizedMaterialGPU {
    uint  shading_model_id;
    uint  feature_mask;
    uint  tex_mask;
    uint  flags;
    uint  _header_pad[4];
    vec4  base_color;
    vec4  toon_params;
    vec4  emissive;
    vec4  _reserved0;
    vec4  _reserved1;
    vec4  _reserved2;
    TextureRefGPU tex[4];
};

// Graph family (Option-B per-material params): a generic blob — 16 vec4 param
// lanes (param slot i -> params[i]) + an 8-entry texture table, indexed by the
// flat vMatIndex like the builtin families. A node-graph material reads BOTH its
// params and its textures from here. Binding 4 = the next ELightingTechnique
// ordinal after the four builtin families (Unlit=0/LegacyLit=1/PBR=2/Stylized=3).
// ABI: the engine-side GraphFamilyGPU MUST match this byte-for-byte. (Hoisted out
// of the material-graph emitter's inline copy into this SSOT so ShaderGen's
// Includer resolves it instead of every backend hand-copying the struct.)
struct GraphMaterialGPU {
    uint  shading_model_id;
    uint  feature_mask;
    uint  tex_mask;
    uint  flags;
    uint  _header_pad[4];
    vec4  params[16];
    TextureRefGPU tex[8];
};

#endif // MATERIAL_TYPES_GLSL
