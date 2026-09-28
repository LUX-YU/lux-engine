#include <lux/engine/editor/scene/detail/ModelCreation.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>

namespace lux::editor::scene::detail
{
    editing::EditResult<std::vector<ObjectContent>> prepareModelCreation(
        const SceneContent& owner,
        const ProjectStorage& project,
        const lux::asset::ModelAsset& asset,
        const Eigen::Vector3d& position,
        lux::partition::PartitionOrdinal partition
    )
    {
        const auto rejected = [](EModelCreationError code, std::string_view message) {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(code),
                message
            ));
        };
        const auto& model = asset.data();
        for (const auto& primitive : model.primitives)
        {
            const auto mesh =
                project.resolveReference(project.reference(primitive.mesh), lux::asset::MeshAsset::primary_magic);
            const auto material = project.resolveReference(
                project.reference(primitive.material),
                lux::asset::MaterialAsset::primary_magic
            );
            if (!mesh || !material)
            {
                return rejected(
                    EModelCreationError::MISSING_DEPENDENCY,
                    "The model references a mesh or material outside the "
                    "project catalog"
                );
            }
        }
        auto prepared = makeSceneModelObjects(asset, position, partition, owner.source.world->data(), owner.metadata);
        if (!prepared) return lux::cxx::unexpected(prepared.error());
        std::vector<ObjectContent> content;
        content.reserve(prepared->size());
        for (auto& object : *prepared)
        {
            ObjectContent captured{object.id, object.partition, {}};
            for (auto& component : object.components)
                captured.components.push_back({owner.metadata.find(component.schema), std::move(component.bytes)});
            content.push_back(std::move(captured));
        }
        return content;
    }
} // namespace lux::editor::scene::detail
