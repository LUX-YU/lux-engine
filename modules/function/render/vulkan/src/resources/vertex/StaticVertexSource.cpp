/**
 * @file StaticVertexSource.cpp
 */

#include <lux/engine/render/resources/vertex/StaticVertexSource.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>

namespace lux::render
{
    StaticVertexSource::StaticVertexSource(
        MeshResources& meshes,
        std::uint16_t vbo_segment,
        VertexLayoutId layout_id
    ) noexcept
        : meshes_(&meshes), vbo_segment_(vbo_segment), layout_id_(layout_id), pool_id_(~0u)
    {
    }

    StaticVertexSource::~StaticVertexSource() = default;

    VkBuffer StaticVertexSource::buffer() const noexcept
    {
        return meshes_->vertexBuffer(vbo_segment_);
    }

    VertexSourceHandle StaticVertexSource::handleForMesh(MeshHandle mesh) const noexcept
    {
        if (!meshes_->alive(mesh))
            return kInvalidVertexSourceHandle;

        const MeshGpuRecord* gpu = meshes_->getGpuRecord(mesh);
        if (!gpu)
            return kInvalidVertexSourceHandle;

        // Mesh must live in this source's segment + match its layout.
        if (gpu->vbo_segment != vbo_segment_)
            return kInvalidVertexSourceHandle;
        if (gpu->layout_id != layout_id_)
            return kInvalidVertexSourceHandle;
        if (gpu->vertex_stride == 0)
            return kInvalidVertexSourceHandle;

        VertexSourceHandle h{};
        h.pool_id = pool_id_;
        h.vertex_base = static_cast<std::uint32_t>(gpu->vertex_buffer_range.offset / gpu->vertex_stride);
        h.vertex_count = static_cast<std::uint32_t>(gpu->vertex_buffer_range.size / gpu->vertex_stride);
        return h;
    }

} // namespace lux::render
