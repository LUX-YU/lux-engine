#ifndef VERTEX_LAYOUT_GLSL
#define VERTEX_LAYOUT_GLSL
// =========================================================================
//  vertex_layout.glsl — vertex-layout specialization-constant contract.
//
//  The bindless vertex pool is a flat float SSBO. Instead of hardcoding the
//  lux::rdesc::Vertex packing, the stride + per-attribute FLOAT offsets are
//  supplied as Vulkan specialization constants, set per pipeline from a
//  registered lux::render::VertexLayout.
//
//  *** This file is the GLSL half of a contract. The C++ half is
//      pipeline/VertexLayoutSpec.hpp — the constant_id numbers AND the default
//      values below MUST stay in sync with it (kVtxSpecInputBase = 110,
//      kVtxSpecOutputBase = 120, vtxSpecSlot()). ***
//
//  Defaults equal layout 0 (= lux::rdesc::Vertex, 22 floats / 88 bytes), so a
//  pipeline that sets NO spec constants behaves byte-for-byte like the old
//  hardcoded path. Offsets are in floats.
//
//  Two blocks:
//   * READ  (110+): the layout a shader READS via luxFetchMeshVertex (the _vp
//     mesh shaders) and the skinning compute's INPUT reads.
//   * OUT   (120+): the skinning compute's OUTPUT write layout.
//     OUT == READ == full layout 0 (22 floats / 88 B). The compute kernel
//     still writes only the geometry prefix; the bone tail is zero-padded
//     so the skinned pool's stride matches the global VBO, letting all
//     _vp.vert pipelines share one spec set.
// =========================================================================

// ---- READ layout (110+) ----
layout(constant_id = 110) const uint LUX_VTX_FLOAT_STRIDE = 22u;
layout(constant_id = 111) const uint LUX_VTX_OFF_POSITION  = 0u;
layout(constant_id = 112) const uint LUX_VTX_OFF_NORMAL    = 3u;
layout(constant_id = 113) const uint LUX_VTX_OFF_TANGENT   = 6u;
layout(constant_id = 114) const uint LUX_VTX_OFF_UV0       = 9u;
layout(constant_id = 115) const uint LUX_VTX_OFF_BITANGENT = 11u;
layout(constant_id = 116) const uint LUX_VTX_OFF_BONE_IDS  = 14u;
layout(constant_id = 117) const uint LUX_VTX_OFF_WEIGHTS   = 18u;

// ---- OUTPUT layout (120+), written by the skinning compute ----
// Defaults aligned with the full READ layout (22 floats / 88 B).
layout(constant_id = 120) const uint LUX_OUT_FLOAT_STRIDE = 22u;
layout(constant_id = 121) const uint LUX_OUT_OFF_POSITION  = 0u;
layout(constant_id = 122) const uint LUX_OUT_OFF_NORMAL    = 3u;
layout(constant_id = 123) const uint LUX_OUT_OFF_TANGENT   = 6u;
layout(constant_id = 124) const uint LUX_OUT_OFF_UV0       = 9u;
layout(constant_id = 125) const uint LUX_OUT_OFF_BITANGENT = 11u;

#endif // VERTEX_LAYOUT_GLSL
