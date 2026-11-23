#pragma once

#include <lux/engine/render/core/ConfigValue.hpp>

#include <lux/engine/render/resource/mesh/Data.hpp>

namespace lux::render
{
    // Authoring metadata for this implementation's actual packed vertex ABI. Ordinary capability
    // users pass Mesh/Material identities; they do not choose the Storage's GPU layout.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<MeshDescription> describeIndexedMesh(const rdesc::Mesh&) noexcept;

    enum class EMeshBuffer : std::uint8_t
    {
        VERTICES,
        INDICES,
        SECTIONS,
        BOUNDS
    };

    struct MeshBinding final
    {
        MeshId mesh;
        EMeshBuffer buffer = EMeshBuffer::VERTICES;
        std::uint32_t stride = 0;
        // Vertex input ABI, shared with indexed draw sources.
        std::vector<GeometryAttribute> attributes;
        bool operator==(const MeshBinding&) const noexcept = default;
    };

    template <> struct ConfigValueTraits<MeshBinding> final
    {
        [[nodiscard]] LUX_RENDER_PUBLIC static RenderResult<std::size_t> retainedBytes(const MeshBinding&) noexcept;
    };
} // namespace lux::render
