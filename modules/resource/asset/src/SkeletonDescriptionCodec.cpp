#include <lux/engine/asset/SkeletonDescriptionCodec.hpp>

#include <lux/engine/asset/detail/ByteIO.hpp>

namespace lux::asset::detail
{
    namespace
    {
        /// Affine3f -> 12 floats: columns 0..3, rows 0..2 (column-major per
        /// Eigen), stripping the implicit final row [0,0,0,1].
        inline void writeAffine3f(ByteWriter& w, const Eigen::Affine3f& m)
        {
            const float* p = m.data();
            float        scratch[12];
            for (int col = 0; col < 4; ++col)
            {
                scratch[col * 3 + 0] = p[col * 4 + 0];
                scratch[col * 3 + 1] = p[col * 4 + 1];
                scratch[col * 3 + 2] = p[col * 4 + 2];
            }
            w.bytes(scratch, sizeof(scratch));
        }

        inline bool readAffine3f(ByteReader& c, Eigen::Affine3f& m) noexcept
        {
            float scratch[12];
            if (!c.bytes(scratch, sizeof(scratch))) return false;
            float* p = m.data();
            for (int col = 0; col < 4; ++col)
            {
                p[col * 4 + 0] = scratch[col * 3 + 0];
                p[col * 4 + 1] = scratch[col * 3 + 1];
                p[col * 4 + 2] = scratch[col * 3 + 2];
                // Last row is implicit [0,0,0,1] for an affine map.
                p[col * 4 + 3] = (col == 3) ? 1.0f : 0.0f;
            }
            return true;
        }
    } // anonymous

    std::vector<std::byte>
    encodeSkeletonDescription(const lux::rdesc::Skeleton& skel)
    {
        ByteWriter w;
        // Reserve a reasonable estimate: header (16B) + per-bone average
        // ~120B (name ~32B + index 4B + 2 mats 96B) + trailer 4B.
        w.reserve(20 + skel.bones.size() * 132);

        w.u32(kSkeletonDescMagic);
        w.u32(kSkeletonEndianTag);
        w.u32(kSkeletonSchemaVersion);
        w.u32(static_cast<std::uint32_t>(skel.bones.size()));

        for (const auto& b : skel.bones)
        {
            w.str(b.name);
            w.i32(b.parent_index);
            writeAffine3f(w, b.bind_local);
            writeAffine3f(w, b.inv_bind_world);
        }

        writeAffine3f(w, skel.global_transform);  // v2: armature prefix for root bones
        w.u32(kSkeletonDescTrailer);
        return std::move(w).take();
    }

    bool decodeSkeletonDescription(std::span<const std::byte> blob,
                                   lux::rdesc::Skeleton&       out,
                                   std::string*                error_out) noexcept
    {
        ByteReader c{blob, error_out};

        std::uint32_t magic = 0, endian = 0, version = 0, bone_count = 0;
        if (!c.u32(magic))   return false;
        if (magic != kSkeletonDescMagic)        { c.fail("bad magic");        return false; }
        if (!c.u32(endian))  return false;
        if (endian != kSkeletonEndianTag)       { c.fail("bad endian tag");   return false; }
        if (!c.u32(version)) return false;
        if (version != kSkeletonSchemaVersion)  { c.fail("schema version mismatch"); return false; }
        if (!c.u32(bone_count)) return false;
        if (bone_count > kMaxSkelBoneCount)     { c.fail("bone count too large"); return false; }

        out.bones.clear();
        out.bones.resize(bone_count);

        for (std::uint32_t i = 0; i < bone_count; ++i)
        {
            auto& b = out.bones[i];
            if (!c.str(b.name, kMaxSkelStringLen)) return false;
            if (!c.i32(b.parent_index))            return false;
            // parent_index validation: -1 or in [0, i). (Forward references
            // would violate the topological-sort invariant Skeleton expects.)
            if (b.parent_index < -1
                || b.parent_index >= static_cast<std::int32_t>(i))
            {
                c.fail("bone parent_index out of range or forward-referencing");
                return false;
            }
            if (!readAffine3f(c, b.bind_local))     return false;
            if (!readAffine3f(c, b.inv_bind_world)) return false;
        }

        if (!readAffine3f(c, out.global_transform)) return false;  // v2

        std::uint32_t trailer = 0;
        if (!c.u32(trailer)) return false;
        if (trailer != kSkeletonDescTrailer) { c.fail("bad trailer"); return false; }

        return c.ok();
    }
} // namespace lux::asset::detail
