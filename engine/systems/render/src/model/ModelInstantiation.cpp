#include <lux/engine/systems/render/model/ModelInstantiation.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/RenderableMeshComponent.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>

namespace lux::engine::render
{
    lux::render::RenderResult<std::vector<ecs::Entity>>
    createModelEntities(ecs::Registry& registry, const rdesc::ModelDescription& model) noexcept
    {
        if (model.root_node >= model.nodes.size())
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
        }
        std::vector<std::uint32_t> order{model.root_node};
        std::vector<bool> visited(model.nodes.size());
        std::vector<Eigen::Affine3f> transforms(model.nodes.size());
        std::vector<ecs::Transform3D> decomposed(model.nodes.size());
        visited[model.root_node] = true;
        transforms[model.root_node] = model.nodes[model.root_node].local_transform;
        std::size_t primitive_count = 0;
        for (std::size_t index = 0; index < order.size(); ++index)
        {
            const auto node_index = order[index];
            const auto& node = model.nodes[node_index];
            for (const auto primitive : node.primitives)
            {
                const bool valid = primitive < model.primitives.size();
                const bool valid_assets = valid && !model.primitives[primitive].mesh.isNull() &&
                    !model.primitives[primitive].material.isNull();
                if (!valid_assets)
                {
                    return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
                }
            }
            primitive_count += node.primitives.size();
            for (const auto child : node.children)
            {
                if (child >= model.nodes.size() || visited[child])
                {
                    return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
                }
                visited[child] = true;
                transforms[child] = transforms[node_index] * model.nodes[child].local_transform;
                order.push_back(child);
            }
            const Eigen::Matrix3d linear = transforms[node_index].linear().cast<double>();
            auto& result = decomposed[node_index];
            result.translation = transforms[node_index].translation().cast<double>();
            result.scale = {linear.col(0).norm(), linear.col(1).norm(), linear.col(2).norm()};
            if (!linear.allFinite() || !result.translation.allFinite() || result.scale.minCoeff() <= 0)
            {
                return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
            }
            if (linear.determinant() < 0)
            {
                result.scale.x() = -result.scale.x();
            }
            const Eigen::Matrix3d rotation = linear * result.scale.cwiseInverse().asDiagonal();
            if (!(rotation.transpose() * rotation).isApprox(Eigen::Matrix3d::Identity(), 1e-5))
            {
                return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
            }
            result.rotation = Eigen::Quaterniond(rotation).normalized();
        }
        std::vector<ecs::Entity> entities;
        entities.reserve(primitive_count);
        for (const auto node : order)
        {
            for (const auto primitive_index : model.nodes[node].primitives)
            {
                const auto& primitive = model.primitives[primitive_index];
                const auto entity = registry.create();
                registry.emplace<ecs::Transform3D>(entity, decomposed[node]);
                registry.emplace<ecs::Mesh3D>(entity, rdesc::MeshVisualDescription{primitive.mesh});
                registry.emplace<ecs::MaterialComponent>(entity, std::vector<asset::AssetId>{primitive.material});
                registry.emplace<ecs::RenderableMeshComponent>(entity);
                entities.push_back(entity);
            }
        }
        return entities;
    }
}
