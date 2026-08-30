#pragma once

#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/toolchain/asset/model/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <Eigen/Geometry>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lux::toolchain
{
    enum class EModelCookError : std::uint8_t
    {
        INVALID_SOURCE,
        INVALID_CONFIGURATION,
        IMPORT_FAILED,
        UNSUPPORTED_FEATURE,
        TEXTURE_COOK_FAILED,
        MATERIAL_COOK_FAILED,
        INVALID_MESH,
        INVALID_SKELETON,
        INVALID_ANIMATION,
        INVALID_MODEL,
        IO_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct ModelCookFailure final
    {
        EModelCookError code{EModelCookError::INVALID_SOURCE};
        std::uint32_t source_ordinal{};
        std::string detail;
    };

    struct ModelCookConfiguration final
    {
        Eigen::Quaternionf pre_rotation{Eigen::Quaternionf::Identity()};
        float uniform_scale{1.0F};
        bool make_left_handed{};
        bool import_animations{true};
    };

    struct ModelCookProduct final
    {
        std::shared_ptr<const lux::asset::ModelAsset> model;
        std::vector<std::shared_ptr<const lux::asset::MeshAsset>> meshes;
        std::vector<std::shared_ptr<const lux::asset::MaterialAsset>> materials;
        std::vector<std::shared_ptr<const lux::asset::TextureAsset>> textures;
        std::optional<std::shared_ptr<const lux::asset::SkeletonAsset>> skeleton;
        std::vector<std::shared_ptr<const lux::asset::AnimationClipAsset>> animations;
    };

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_MODEL_PUBLIC lux::cxx::expected<
        ModelCookProduct,
        ModelCookFailure
    > cookModel(
        lux::asset::AssetInfo model_info,
        const std::filesystem::path& source,
        const ModelCookConfiguration& configuration = {}
    ) noexcept;
} // namespace lux::toolchain
