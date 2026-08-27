#pragma once
#include <Eigen/Core>
#include <cstdint>

namespace lux::rdesc
{
    /**
     * @brief Maximum number of bones that can influence a single vertex.
     */
    static inline constexpr uint8_t max_bone_influence = 4;

    /**
     * @brief Structure representing bone influence data for skeletal animation.
     *
     * Each bone influence contains up to max_bone_influence bone IDs and their
     * corresponding weights that affect vertex deformation during animation.
     */
    struct Bone
    {
        int bone_ids[max_bone_influence];  ///< Array of bone identifiers
        float weights[max_bone_influence]; ///< Array of influence weights
    };

    /**
     * @brief Structure representing a single vertex in a mesh.
     *
     * A vertex contains all the necessary information for rendering including
     * position, normal, tangent space vectors, and texture coordinates.
     */
    struct Vertex
    {
        Eigen::Vector3f position; ///< 3D position in local space
        Eigen::Vector3f normal;   ///< Surface normal vector
        Eigen::Vector3f tangent;  ///< Tangent vector for normal mapping
        Eigen::Vector2f uv;       ///< Texture coordinates

        Eigen::Vector3f bitangent; ///< Bitangent vector for normal mapping
        Bone bone;                 ///< Per-vertex bone influence (4 ids + 4 weights)
    };
}
