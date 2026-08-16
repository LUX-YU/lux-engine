#pragma once
#include <cstdint>

namespace lux::render
{
    // ===== std430 utility types =====

    // C++ stand-ins for GLSL vector types under std430. The alignment in each
    // name is the one std430 actually requires — that is the whole point of
    // these types, so a mirror struct laid out with them matches the shader's
    // view field for field.
    //
    //   GLSL vec2 → align 8,  size 8
    //   GLSL vec3 → align 16, size 12 (alignas(16) rounds the C++ size to 16,
    //                                  which is what the next field's alignment
    //                                  would have produced anyway)
    //   GLSL vec4 → align 16, size 16
    //
    // (There used to be an `aligned16vec2` here. It corresponded to no std430
    //  rule — vec2 aligns to 8 — and its single user, AreaLightGPU::size, was
    //  laid out 8 bytes off from the shader's AreaLightGPU from that field on.)
    struct alignas(8)  aligned8vec2  { float x, y; };
    struct alignas(16) aligned16vec3 { float x, y, z; };
    struct alignas(16) aligned16vec4 { float x, y, z, w; };
}