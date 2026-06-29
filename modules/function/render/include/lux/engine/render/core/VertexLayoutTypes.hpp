#pragma once
// Lightweight leaf header exposing only the VertexLayoutId type alias.
// This avoids pulling gapi/vk/vk.hpp into headers that only need the ID type
// (e.g. MeshResources.hpp, GpuMesh struct).
//
// Full VertexLayout / VertexAttribute types (which require VkFormat) are in
// <lux/engine/render/pipeline/VertexLayout.hpp>.
#include <cstdint>
#include <limits>

namespace lux::render
{
    /// Opaque identifier for a registered vertex attribute layout.
    using VertexLayoutId = uint32_t;

    /// Sentinel value for an unregistered / unset layout ID.
    inline constexpr VertexLayoutId kInvalidVertexLayoutId =
        std::numeric_limits<VertexLayoutId>::max();

    /// Standard layouts registered at startup (RenderServer init).
    inline constexpr VertexLayoutId kDefaultVertexLayoutId     = 0;  ///< = rdesc::Vertex (full, incl. bone)
    inline constexpr VertexLayoutId kLeanSkinnedVertexLayoutId = 1;  ///< geometry-only skinned output
}
