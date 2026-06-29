#pragma once
#include <Eigen/Geometry>
#include <string>
#include <vector>
#include <cstdint>

namespace lux::rdesc
{
    /**
     * @brief Structure representing a single bone in a skeleton hierarchy.
     *
     * One Bone_t is one named joint in the rest pose tree. `parent_index`
     * is an index into the owning Skeleton's `bones` array, or -1 for the
     * root bone. The two transforms split the rest-pose information into
     * the two forms that runtime skinning actually needs:
     *   - `bind_local`     — local-space rest pose (relative to parent).
     *                        Used at runtime to compose world-space bone
     *                        transforms when no animation is overriding
     *                        the bone (it is the value AnimationSystem
     *                        falls back to for bones not present in a
     *                        clip).
     *   - `inv_bind_world` — inverse of the world-space rest transform.
     *                        Multiplied by the animated world transform to
     *                        get the skinning matrix that takes a vertex
     *                        from rest pose to its animated position.
     *                        Equal to `aiBone::mOffsetMatrix` for Assimp
     *                        imports.
     *
     * Suffix `_t` distinguishes this from the per-vertex `struct Bone`
     * (bone influences + weights) declared in Vertex.hpp; the two are
     * different concepts that historically share a name.
     */
    struct Bone_t
    {
        std::string       name;            ///< Bone name (matches Assimp / DCC tool)
        int32_t           parent_index;    ///< Index into Skeleton::bones, or -1 for root
        Eigen::Affine3f   bind_local;      ///< Local-space rest transform (relative to parent)
        Eigen::Affine3f   inv_bind_world;  ///< Inverse of world-space rest transform
    };

    /**
     * @brief Bone hierarchy + rest pose of a skeletal mesh.
     *
     * A Skeleton is independent from any specific mesh — a Mesh references
     * a Skeleton by asset reference, and a Skeleton can be reused across
     * multiple meshes (typical for a character with detachable equipment).
     *
     * Bones SHOULD be ordered so that any bone's `parent_index` is less
     * than its own index. AnimationSystem's `buildSkinningMatrices` relies
     * on a single in-order pass over `bones`; importers should topologically
     * sort during asset_pipeline export.
     */
    struct Skeleton
    {
        std::vector<Bone_t> bones; ///< Bone array; bone i's parent is bones[bones[i].parent_index]

        /// World-space prefix applied to ROOT bones (parent_index < 0): the
        /// accumulated transform of the non-bone nodes (armature / empty objects)
        /// sitting between the model's scene root and the skeleton's root bone,
        /// with the scene-root transform cancelled out (Assimp globalInverse).
        /// Identity in the common case where the root bone hangs directly under
        /// the scene root. `buildSkinningMatrices` left-multiplies this onto each
        /// root bone so models with a transformed armature skin correctly.
        Eigen::Affine3f global_transform{ Eigen::Affine3f::Identity() };
    };
} // namespace lux::rdesc
