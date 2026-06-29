#include <lux/engine/asset/AnimationClipDescriptionCodec.hpp>

#include <lux/engine/asset/detail/ByteIO.hpp>

#include <algorithm>
#include <cmath>

namespace lux::asset::detail
{
    namespace
    {
        // -----------------------------------------------------------------
        //  unorm16 quaternion quantization.
        //  Each component q in [-1, 1] maps to u16 in [0, 65535] linearly:
        //    encode:  u = round((q + 1) * 0.5 * 65535)
        //    decode:  q = u * (2 / 65535) - 1
        //  Max per-component error ~1.5e-5; decode renormalizes the quat.
        // -----------------------------------------------------------------
        inline constexpr float kU16Scale  = 65535.0f;
        inline constexpr float kU16Inv2   = 2.0f / 65535.0f;

        constexpr std::uint16_t encodeUnorm16Pm1(float v) noexcept
        {
            const float n = std::clamp(v, -1.0f, 1.0f);
            const float scaled = (n + 1.0f) * 0.5f * kU16Scale;
            // +0.5 round-to-nearest (always positive, so truncation == round).
            return static_cast<std::uint16_t>(scaled + 0.5f);
        }

        constexpr float decodeUnorm16Pm1(std::uint16_t u) noexcept
        {
            return static_cast<float>(u) * kU16Inv2 - 1.0f;
        }

        inline void writeVec3(ByteWriter& w, const Eigen::Vector3f& v)
        {
            w.f32(v.x()); w.f32(v.y()); w.f32(v.z());
        }

        inline void writeQuatUnorm16(ByteWriter& w, const Eigen::Quaternionf& q)
        {
            // Order matches Eigen storage; we serialize as (x, y, z, w),
            // matching the conventional wire order (glTF, Unity, Unreal).
            // We assume the input quaternion is already unit-norm — Assimp /
            // glTF guarantee this; runtime decode renormalizes anyway.
            w.u16(encodeUnorm16Pm1(q.x()));
            w.u16(encodeUnorm16Pm1(q.y()));
            w.u16(encodeUnorm16Pm1(q.z()));
            w.u16(encodeUnorm16Pm1(q.w()));
        }

        inline bool readVec3(ByteReader& c, Eigen::Vector3f& v) noexcept
        {
            float x, y, z;
            if (!c.f32(x) || !c.f32(y) || !c.f32(z)) return false;
            v = Eigen::Vector3f(x, y, z);
            return true;
        }

        inline bool readQuatUnorm16(ByteReader& c, Eigen::Quaternionf& q) noexcept
        {
            // Rotation block on disk is 4 unorm16 components (x,y,z,w).
            // Dequantize then renormalize to bound the rounding error
            // (worst-case per-component ~1.5e-5 before renormalize).
            std::uint16_t ux, uy, uz, uw;
            if (!c.u16(ux) || !c.u16(uy) || !c.u16(uz) || !c.u16(uw))
                return false;
            const float x = decodeUnorm16Pm1(ux);
            const float y = decodeUnorm16Pm1(uy);
            const float z = decodeUnorm16Pm1(uz);
            const float w = decodeUnorm16Pm1(uw);
            Eigen::Quaternionf qq(w, x, y, z); // Eigen ctor takes (w, x, y, z)
            qq.normalize();                     // renormalize — slerp wants unit quats
            q = qq;
            return true;
        }

        // Read a key-count and validate against the per-track cap.
        inline bool readKeyCount(ByteReader& c, std::uint32_t& n) noexcept
        {
            if (!c.u32(n)) return false;
            if (n > kMaxAcKeyCount) { c.fail("track key count too large"); return false; }
            return true;
        }

        // -----------------------------------------------------------------
        //  Constant-track folding. Encoder collapses any channel
        //  whose keys are all equal within `eps` to a single key — the
        //  decoder reads it as a normal 1-key track and the runtime
        //  sampler clamps any t to that key's value.
        // -----------------------------------------------------------------
        inline constexpr float kConstTrackEpsVec3 = 1.0e-5f;
        inline constexpr float kConstTrackEpsQuat = 1.0e-5f;

        inline bool isConstantVec3Track(const std::vector<Eigen::Vector3f>& v) noexcept
        {
            const std::size_t n = v.size();
            if (n <= 1) return false;  // already minimal — no work to do
            const Eigen::Vector3f& first = v.front();
            for (std::size_t i = 1; i < n; ++i)
                if ((v[i] - first).squaredNorm() > kConstTrackEpsVec3 * kConstTrackEpsVec3)
                    return false;
            return true;
        }

        inline bool isConstantQuatTrack(const std::vector<Eigen::Quaternionf>& q) noexcept
        {
            const std::size_t n = q.size();
            if (n <= 1) return false;
            const Eigen::Quaternionf& first = q.front();
            for (std::size_t i = 1; i < n; ++i)
            {
                // q and -q represent the same rotation; align signs before
                // comparing component-wise.
                Eigen::Quaternionf aligned = q[i];
                if (first.dot(aligned) < 0.0f)
                    aligned.coeffs() = -aligned.coeffs();
                if (!aligned.coeffs().isApprox(first.coeffs(), kConstTrackEpsQuat))
                    return false;
            }
            return true;
        }
    } // anonymous

    std::vector<std::byte>
    encodeAnimationClipDescription(const lux::rdesc::AnimationClip& clip)
    {
        ByteWriter w;
        // Rough size estimate — header + name + per-track avg (~ index 4B
        // + 3 counts 12B + 3 time arrays + 3 value arrays).
        std::size_t key_estimate = 0;
        for (const auto& t : clip.tracks)
            key_estimate += t.times_t.size() + t.times_r.size() + t.times_s.size();
        w.reserve(64 + clip.name.size()
                     + clip.tracks.size() * 32
                     + key_estimate * 20);

        w.u32(kAcDescMagic);
        w.u32(kAcEndianTag);
        w.u32(kAcSchemaVersion);
        w.str(clip.name);
        w.f32(clip.duration);
        w.u8 (clip.loop ? 1 : 0);
        w.u8 (0); // reserved
        w.u8 (0); // reserved
        w.u8 (0); // reserved
        w.u32(static_cast<std::uint32_t>(clip.tracks.size()));

        for (const auto& t : clip.tracks)
        {
            w.i32(t.bone_index);

            // Constant-track folding: if every key in a channel is
            // equal-within-eps to the first, write only the first key. The
            // runtime sampler already clamps to the head key for any
            // t <= times[0], so this is wire-only (no decoder change).
            const bool t_const = isConstantVec3Track(t.translations)
                                 && t.times_t.size() == t.translations.size();
            const bool r_const = isConstantQuatTrack(t.rotations)
                                 && t.times_r.size() == t.rotations.size();
            const bool s_const = isConstantVec3Track(t.scales)
                                 && t.times_s.size() == t.scales.size();

            // Translation channel.
            const std::uint32_t n_t = t_const
                ? 1u
                : static_cast<std::uint32_t>(t.times_t.size());
            w.u32(n_t);
            if (t_const)
            {
                w.f32(t.times_t.empty() ? 0.0f : t.times_t.front());
                writeVec3(w, t.translations.front());
            }
            else
            {
                for (float k : t.times_t)              w.f32(k);
                for (const auto& v : t.translations)   writeVec3(w, v);
            }

            // Rotation channel (unorm16 components on the wire).
            const std::uint32_t n_r = r_const
                ? 1u
                : static_cast<std::uint32_t>(t.times_r.size());
            w.u32(n_r);
            if (r_const)
            {
                w.f32(t.times_r.empty() ? 0.0f : t.times_r.front());
                writeQuatUnorm16(w, t.rotations.front());
            }
            else
            {
                for (float k : t.times_r)              w.f32(k);
                for (const auto& q : t.rotations)      writeQuatUnorm16(w, q);
            }

            // Scale channel.
            const std::uint32_t n_s = s_const
                ? 1u
                : static_cast<std::uint32_t>(t.times_s.size());
            w.u32(n_s);
            if (s_const)
            {
                w.f32(t.times_s.empty() ? 0.0f : t.times_s.front());
                writeVec3(w, t.scales.front());
            }
            else
            {
                for (float k : t.times_s)              w.f32(k);
                for (const auto& v : t.scales)         writeVec3(w, v);
            }
        }

        w.u32(kAcDescTrailer);
        return std::move(w).take();
    }

    bool decodeAnimationClipDescription(std::span<const std::byte>      blob,
                                        lux::rdesc::AnimationClip&       out,
                                        std::string*                     error_out) noexcept
    {
        ByteReader c{blob, error_out};

        std::uint32_t magic = 0, endian = 0, version = 0, track_count = 0;
        if (!c.u32(magic))   return false;
        if (magic != kAcDescMagic)                  { c.fail("bad magic");        return false; }
        if (!c.u32(endian))  return false;
        if (endian != kAcEndianTag)                 { c.fail("bad endian tag");   return false; }
        if (!c.u32(version)) return false;
        if (version != kAcSchemaVersion)            { c.fail("schema version mismatch"); return false; }

        if (!c.str(out.name, kMaxAcStringLen)) return false;
        if (!c.f32(out.duration)) return false;

        std::uint8_t loop_u8 = 0;
        if (!c.u8(loop_u8)) return false;
        out.loop = (loop_u8 != 0);
        // Skip 3 reserved bytes.
        std::uint8_t r0, r1, r2;
        if (!c.u8(r0) || !c.u8(r1) || !c.u8(r2)) return false;
        (void)r0; (void)r1; (void)r2;

        if (!c.u32(track_count)) return false;
        if (track_count > kMaxAcTrackCount) { c.fail("track count too large"); return false; }

        out.tracks.clear();
        out.tracks.resize(track_count);

        for (std::uint32_t i = 0; i < track_count; ++i)
        {
            auto& t = out.tracks[i];
            if (!c.i32(t.bone_index)) return false;

            // Translation track.
            std::uint32_t n_t = 0;
            if (!readKeyCount(c, n_t)) return false;
            t.times_t.resize(n_t);
            for (auto& k : t.times_t) if (!c.f32(k)) return false;
            t.translations.resize(n_t);
            for (auto& v : t.translations) if (!readVec3(c, v)) return false;

            // Rotation track (rotation block is unorm16 on disk).
            std::uint32_t n_r = 0;
            if (!readKeyCount(c, n_r)) return false;
            t.times_r.resize(n_r);
            for (auto& k : t.times_r) if (!c.f32(k)) return false;
            t.rotations.resize(n_r);
            for (auto& q : t.rotations) if (!readQuatUnorm16(c, q)) return false;

            // Scale track.
            std::uint32_t n_s = 0;
            if (!readKeyCount(c, n_s)) return false;
            t.times_s.resize(n_s);
            for (auto& k : t.times_s) if (!c.f32(k)) return false;
            t.scales.resize(n_s);
            for (auto& v : t.scales) if (!readVec3(c, v)) return false;
        }

        std::uint32_t trailer = 0;
        if (!c.u32(trailer)) return false;
        if (trailer != kAcDescTrailer) { c.fail("bad trailer"); return false; }

        return c.ok();
    }
} // namespace lux::asset::detail
