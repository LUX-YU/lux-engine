#include <lux/engine/resource/asset/MeshDescriptionCodec.hpp>

#include <lux/engine/core/serialization/ByteIO.hpp>

namespace lux::asset::detail
{
    using lux::core::serialization::ByteReader;
    using lux::core::serialization::ByteWriter;
    // The codec bulk-copies the vertex/index arrays as raw bytes (meshes are
    // large — element-wise float writes would be needlessly slow). Vertex is a
    // flat aggregate of floats/ints (Eigen fixed-size vectors hold contiguous
    // floats, no pointers/heap), so it is bitwise-serializable — the same
    // assumption the GPU upload path already relies on. NOTE: Eigen declares a
    // non-trivial copy ctor, so std::is_trivially_copyable is FALSE even though
    // the bytes copy fine; we therefore guard on size: any field change flips
    // sizeof and fails here → bump kMeshSchemaVersion + revisit the byte layout.
    static_assert(sizeof(lux::rdesc::Vertex) == 88,
                  "Vertex layout changed — bump kMeshSchemaVersion and revisit the codec.");

    std::vector<std::byte>
    encodeMeshDescription(const lux::rdesc::Mesh& mesh)
    {
        const std::size_t vcount = mesh.vertices.size();
        const std::size_t icount = mesh.indices.size();

        ByteWriter w;
        w.reserve(24
                  + vcount * sizeof(lux::rdesc::Vertex)
                  + icount * sizeof(std::uint32_t)
                  + 28);

        w.u32(kMeshDescMagic);
        w.u32(kMeshEndianTag);
        w.u32(kMeshSchemaVersion);
        w.u32(static_cast<std::uint32_t>(vcount));
        w.u32(static_cast<std::uint32_t>(icount));
        w.u32(mesh.bounds.has_value() ? 1u : 0u);

        if (vcount > 0)
            w.bytes(mesh.vertices.data(), vcount * sizeof(lux::rdesc::Vertex));
        if (icount > 0)
            w.bytes(mesh.indices.data(), icount * sizeof(std::uint32_t));

        if (mesh.bounds.has_value())
        {
            const lux::math::AABB& b = *mesh.bounds;
            w.f32(b.min.x()); w.f32(b.min.y()); w.f32(b.min.z());
            w.f32(b.max.x()); w.f32(b.max.y()); w.f32(b.max.z());
        }

        // LOD chain (schema v2+). LOD0 is the `indices` block above; `lods` holds
        // LOD1..N as reduced index lists over the SAME vertices.
        w.u32(static_cast<std::uint32_t>(mesh.lods.size()));
        for (const auto& lod : mesh.lods)
        {
            w.u32(static_cast<std::uint32_t>(lod.indices.size()));
            w.f32(lod.error);
            if (!lod.indices.empty())
                w.bytes(lod.indices.data(), lod.indices.size() * sizeof(std::uint32_t));
        }

        w.u32(kMeshDescTrailer);
        return std::move(w).take();
    }

    bool decodeMeshDescription(std::span<const std::byte> blob,
                               lux::rdesc::Mesh&          out,
                               std::string*               error_out) noexcept
    {
        ByteReader c{blob, error_out};

        std::uint32_t magic = 0, endian = 0, version = 0;
        std::uint32_t vcount = 0, icount = 0, has_bounds = 0;

        if (!c.u32(magic))   return false;
        if (magic != kMeshDescMagic)       { c.fail("bad magic");        return false; }
        if (!c.u32(endian))  return false;
        if (endian != kMeshEndianTag)      { c.fail("bad endian tag");   return false; }
        if (!c.u32(version)) return false;
        // Tolerant: accept any version in [1, current]. v1 blobs simply carry no
        // LOD chain (single-LOD mesh) — no forced re-bake of existing assets.
        if (version < 1u || version > kMeshSchemaVersion)
        { c.fail("schema version mismatch"); return false; }
        if (!c.u32(vcount))  return false;
        if (vcount > kMaxMeshVertexCount)  { c.fail("vertex count too large"); return false; }
        if (!c.u32(icount))  return false;
        if (icount > kMaxMeshIndexCount)   { c.fail("index count too large"); return false; }
        if (!c.u32(has_bounds)) return false;

        out.vertices.clear();
        out.indices.clear();
        out.bounds.reset();
        out.lods.clear();

        out.vertices.resize(vcount);
        if (vcount > 0 &&
            !c.bytes(out.vertices.data(), vcount * sizeof(lux::rdesc::Vertex)))
            return false;

        out.indices.resize(icount);
        if (icount > 0 &&
            !c.bytes(out.indices.data(), icount * sizeof(std::uint32_t)))
            return false;

        if (has_bounds != 0)
        {
            lux::math::AABB box;
            if (!c.f32(box.min.x()) || !c.f32(box.min.y()) || !c.f32(box.min.z()) ||
                !c.f32(box.max.x()) || !c.f32(box.max.y()) || !c.f32(box.max.z()))
                return false;
            out.bounds = box;
        }

        // LOD chain — only present in schema v2+. v1 blobs decode with no LODs
        // (single-LOD mesh) — tolerant by design, no re-bake of existing assets.
        if (version >= 2u)
        {
            std::uint32_t lod_count = 0;
            if (!c.u32(lod_count)) return false;
            if (lod_count > kMaxMeshLodCount) { c.fail("lod count too large"); return false; }
            out.lods.resize(lod_count);
            for (auto& lod : out.lods)
            {
                std::uint32_t lic = 0;
                if (!c.u32(lic)) return false;
                if (lic > kMaxMeshIndexCount) { c.fail("lod index count too large"); return false; }
                if (!c.f32(lod.error)) return false;
                lod.indices.resize(lic);
                if (lic > 0 &&
                    !c.bytes(lod.indices.data(), lic * sizeof(std::uint32_t)))
                    return false;
            }
        }

        std::uint32_t trailer = 0;
        if (!c.u32(trailer)) return false;
        if (trailer != kMeshDescTrailer) { c.fail("bad trailer"); return false; }

        return c.ok();
    }
} // namespace lux::asset::detail
