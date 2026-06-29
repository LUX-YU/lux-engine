#pragma once
// ============================================================================
//  GraphMaterialData.hpp — flat, trivially-copyable description of a node-graph
//  material instance (the "Graph" family, ELightingTechnique::Graph).
//
//  A graph material is a generic data blob: per-material param lanes + a small
//  texture table. The graph-generated fragment shader reads these from the
//  Graph family SSBO (set 4, binding 4) indexed by vMatIndex. This struct is the
//  CPU-side input to MaterialResources::submitGraph() and is also sent verbatim
//  over the render comm channel (so it MUST stay trivially copyable / POD — no
//  pointers, vectors, or std::optional).
//
//  Slot indices are the graph's declared param/texture order: param slot i maps
//  to the GraphFamilyGPU params[i] lane (swizzled to the slot's arity by the
//  compiler); texture slot i maps to tex[i].
// ============================================================================

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct GraphMaterialData
    {
        static constexpr uint32_t kMaxParams   = 16;  // matches GraphFamilyGPU::params
        static constexpr uint32_t kMaxTextures = 8;   // matches GraphFamilyGPU::tex

        // param slot i -> params[i] = {x,y,z,w}; unused lanes stay zero.
        float    params[kMaxParams][4]{};
        // texture slot i -> bindless index (RTextureHandle.index); unused stay 0.
        uint32_t tex_bindless[kMaxTextures]{};

        uint32_t tex_mask{ 0 };      // bit i set if texture slot i is bound
        uint32_t param_count{ 0 };   // number of declared param lanes actually used
        uint32_t flags{ 0 };         // reserved (MATF_* style)
        uint32_t _pad{ 0 };
    };
    static_assert(std::is_trivially_copyable_v<GraphMaterialData>,
                  "GraphMaterialData is sent as raw bytes over the comm channel");

} // namespace lux::render
