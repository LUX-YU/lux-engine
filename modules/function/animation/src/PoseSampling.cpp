/**
 * @file PoseSampling.cpp
 * @brief Pose sampling + skinning matrix composition.
 */

#include <lux/engine/animation/PoseSampling.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace lux::animation
{
    namespace
    {
        // ---- time normalization -----------------------------------------------

        /// Map @p time to [0, duration]. Loops if @p loop, else clamps.
        /// If duration <= 0, returns 0 (treat as static pose).
        float normalizeTime(float time, float duration, bool loop) noexcept
        {
            if (!(duration > 0.0f))
                return 0.0f;
            if (loop)
            {
                float wrapped = std::fmod(time, duration);
                if (wrapped < 0.0f)
                    wrapped += duration; // negative time → forward wrap
                return wrapped;
            }
            if (time < 0.0f)
                return 0.0f;
            if (time > duration)
                return duration;
            return time;
        }

        // ---- per-channel sampling ---------------------------------------------

        /// Returns alpha in [0,1] + the two key indices spanning @p time.
        /// Caller guarantees @p times.size() >= 2.
        struct SpanResult
        {
            std::size_t lo;
            std::size_t hi;
            float alpha;
        };

        SpanResult findSpan(const std::vector<float>& times, float time) noexcept
        {
            // upper_bound: first key strictly > time.
            auto it = std::upper_bound(times.begin(), times.end(), time);
            auto hi = static_cast<std::size_t>(it - times.begin());
            if (hi == 0)
                return {0, 0, 0.0f}; // before first
            if (hi == times.size())
                return {hi - 1, hi - 1, 0.0f}; // at/after last
            std::size_t lo = hi - 1;
            float t0 = times[lo];
            float t1 = times[hi];
            float a = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
            return {lo, hi, a};
        }

        Eigen::Vector3f sampleVec3(
            const std::vector<float>& times,
            const std::vector<Eigen::Vector3f>& values,
            float time,
            const Eigen::Vector3f& fallback
        ) noexcept
        {
            if (values.empty())
                return fallback;
            if (values.size() == 1)
                return values.front();
            auto s = findSpan(times, time);
            if (s.lo == s.hi)
                return values[s.lo];
            return values[s.lo] + s.alpha * (values[s.hi] - values[s.lo]);
        }

        Eigen::Quaternionf sampleQuat(
            const std::vector<float>& times,
            const std::vector<Eigen::Quaternionf>& values,
            float time,
            const Eigen::Quaternionf& fallback
        ) noexcept
        {
            if (values.empty())
                return fallback;
            if (values.size() == 1)
                return values.front();
            auto s = findSpan(times, time);
            if (s.lo == s.hi)
                return values[s.lo];
            // Eigen's slerp picks the shortest arc internally.
            return values[s.lo].slerp(s.alpha, values[s.hi]);
        }

        // ---- bind-pose decomposition ------------------------------------------

        /// Decompose an Affine3f into translation / rotation / scale, assuming
        /// the linear part is a pure rotation*scale (no shear). Column norms
        /// are interpreted as per-axis scale; rows divided by their norm give
        /// the rotation matrix. Robust for typical rigged-skeleton bind poses.
        void decomposeBindLocal(
            const Eigen::Affine3f& m,
            Eigen::Vector3f& out_t,
            Eigen::Quaternionf& out_r,
            Eigen::Vector3f& out_s
        ) noexcept
        {
            out_t = m.translation();
            Eigen::Matrix3f L = m.linear();
            out_s = Eigen::Vector3f(L.col(0).norm(), L.col(1).norm(), L.col(2).norm());
            Eigen::Matrix3f R = L;
            for (int i = 0; i < 3; ++i)
            {
                if (out_s[i] > 0.0f)
                    R.col(i) /= out_s[i];
            }
            out_r = Eigen::Quaternionf(R);
            out_r.normalize();
        }
    } // namespace

    // -------------------------------------------------------------------------
    // precomputeBindLocals
    // -------------------------------------------------------------------------

    void precomputeBindLocals(const lux::rdesc::Skeleton& skeleton, std::vector<BindLocalTRS>& out)
    {
        const std::size_t n = skeleton.bones.size();
        out.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            decomposeBindLocal(
                skeleton.bones[i].bind_local,
                out[i].translation,
                out[i].rotation,
                out[i].scale
            );
    }

    // -------------------------------------------------------------------------
    // samplePose
    // -------------------------------------------------------------------------

    void samplePose(
        const lux::rdesc::AnimationClip& clip,
        const lux::rdesc::Skeleton& skeleton,
        float time,
        bool loop,
        lux::rdesc::Pose& out_pose,
        std::span<const BindLocalTRS> bind_trs
    )
    {
        const std::size_t n = skeleton.bones.size();

        // Seed every bone with its bind-local; tracked bones overwrite below.
        // resize (not assign(n, Identity)) — the loop writes all n entries, so
        // a separate Identity fill would just be discarded.
        out_pose.bone_local.resize(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            out_pose.bone_local[i] = skeleton.bones[i].bind_local;
        }

        const bool have_cache = bind_trs.size() == n;
        const float t = normalizeTime(time, clip.duration, loop);

        for (const auto& track : clip.tracks)
        {
            if (track.bone_index < 0)
                continue;
            const auto idx = static_cast<std::size_t>(track.bone_index);
            if (idx >= n)
                continue; // stale track, skeleton edited after baking

            // Per-channel bind fallback (a track may omit a channel). Use the
            // precomputed decomposition when supplied; otherwise decompose the
            // static bind pose on demand (correct but recomputes every call).
            Eigen::Vector3f bind_t, bind_s;
            Eigen::Quaternionf bind_r;
            if (have_cache)
            {
                bind_t = bind_trs[idx].translation;
                bind_r = bind_trs[idx].rotation;
                bind_s = bind_trs[idx].scale;
            }
            else
            {
                decomposeBindLocal(skeleton.bones[idx].bind_local, bind_t, bind_r, bind_s);
            }

            const Eigen::Vector3f tr = sampleVec3(track.times_t, track.translations, t, bind_t);
            const Eigen::Quaternionf rt = sampleQuat(track.times_r, track.rotations, t, bind_r);
            const Eigen::Vector3f sc = sampleVec3(track.times_s, track.scales, t, bind_s);

            // Recompose: M = T * R * S
            Eigen::Affine3f m = Eigen::Affine3f::Identity();
            m.translate(tr).rotate(rt).scale(sc);
            out_pose.bone_local[idx] = m;
        }
    }

    // -------------------------------------------------------------------------
    // buildSkinningMatrices
    // -------------------------------------------------------------------------

    void buildSkinningMatrices(
        const lux::rdesc::Pose& pose,
        const lux::rdesc::Skeleton& skeleton,
        std::vector<Eigen::Matrix4f>& out_matrices
    )
    {
        const std::size_t n = skeleton.bones.size();
        out_matrices.assign(n, Eigen::Matrix4f::Identity());

        if (pose.bone_local.size() != n)
        {
            // Mismatch — caller wired a pose against the wrong skeleton.
            // Return identities so the renderer falls back to bind pose
            // rather than producing NaNs.
            return;
        }

        // Compose world-space transforms in topological order. Phase 1's
        // SkeletonAsset import guarantees parent_index[i] < i.
        // Reused scratch (thread_local) so steady-state animation does not heap-
        // allocate per frame; .assign reuses capacity. Animation update is
        // single-threaded today; thread_local keeps it safe if parallelized later.
        thread_local std::vector<Eigen::Affine3f> world_local;
        world_local.assign(n, Eigen::Affine3f::Identity());
        for (std::size_t i = 0; i < n; ++i)
        {
            const int32_t p = skeleton.bones[i].parent_index;
            assert(p < static_cast<int32_t>(i) && "Skeleton must be topologically sorted (parent_index < self_index)");
            if (p >= 0 && static_cast<std::size_t>(p) < i)
            {
                world_local[i] = world_local[static_cast<std::size_t>(p)] * pose.bone_local[i];
            }
            else
            {
                // Root bone: left-multiply the skeleton's global (armature) prefix.
                // Identity in the common case → result unchanged.
                world_local[i] = skeleton.global_transform * pose.bone_local[i];
            }
            out_matrices[i] = (world_local[i] * skeleton.bones[i].inv_bind_world).matrix();
        }
    }
}
