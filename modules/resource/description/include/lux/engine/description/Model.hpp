#pragma once

#include <lux/engine/resource/identity/AssetId.hpp>

#include <Eigen/Geometry>

#include <cstdint>
#include <optional>
#include <vector>

namespace lux::rdesc
{
    struct ModelPrimitive final
    {
        lux::asset::AssetId mesh;
        lux::asset::AssetId material;
    };

    struct ModelNode final
    {
        Eigen::Affine3f local_transform{Eigen::Affine3f::Identity()};
        std::vector<std::uint32_t> primitives;
        std::vector<std::uint32_t> children;
    };

    struct ModelDescription final
    {
        std::uint32_t root_node{};
        std::vector<ModelPrimitive> primitives;
        std::vector<ModelNode> nodes;
        std::optional<lux::asset::AssetId> skeleton;
        std::vector<lux::asset::AssetId> animations;
    };
} // namespace lux::rdesc
