#include <lux/engine/toolchain/scene/MeshSceneContent.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineMetaFunctionRegistry.hpp>
#include <lux/engine/GeometryDeclaration.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/ecs/TransformEvaluation.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/VisualSchema.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>

#include <algorithm>
#include <cmath>

namespace lux::engine::toolchain
{
    namespace
    {
        constexpr auto kInvalid = error::errorId("lux.scene.content.invalid");
        constexpr auto kSchema = error::errorId("lux.scene.content.schema");

        void
        visitMeshSceneObjects(
            const ecs::Registry& registry, void* state, void (*visit)(void*, ecs::Entity) noexcept
        ) noexcept
        {
            for (auto entity : registry.view<const ecs::Mesh3D, const ecs::Transform3D>()) visit(state, entity);
        }

        cxx::expected<std::pair<ecs::Mesh3D, ecs::Transform3D>, error::Error>
        decodeMeshSceneFields(const scene::SceneObjectData& object) noexcept
        {
            std::pair<ecs::Mesh3D, ecs::Transform3D> value;
            bool mesh = false;
            bool transform = false;
            const auto decode = [&](const scene::SceneComponentData& component, const auto& schemas, auto& output)
            {
                using Value = std::remove_reference_t<decltype(output)>;
                const auto type = cxx::make_basic_type_info<Value>();
                auto schema = std::ranges::find(schemas, type, &ecs::ComponentSchema::cpp_type);
                if (schema == schemas.end() || schema->id != component.schema || !schema->decode_value) return false;
                auto decoded = schema->decode_value(component.version, component.payload, {}, {});
                if (!decoded) return false;
                output = *static_cast<const Value*>(decoded->data());
                return true;
            };
            for (const auto& component : object.components)
            {
                if (component.schema.name == "lux.ecs.Parent") return cxx::unexpected(error::Error{kSchema});
                if (component.schema.name == "lux.ecs.Mesh3D")
                {
                    if (mesh || !decode(component, ecs::visualComponentSchemas(), value.first))
                        return cxx::unexpected(error::Error{kSchema});
                    mesh = true;
                }
                if (component.schema.name == "lux.ecs.Transform3D")
                {
                    if (transform || !decode(component, ecs::transformComponentSchemas(), value.second))
                        return cxx::unexpected(error::Error{kSchema});
                    transform = true;
                }
            }
            if (!mesh || !transform) return cxx::unexpected(error::Error{kSchema});
            return value;
        }

        cxx::expected<std::vector<spatial::Spatial3DQueryBounds>, error::Error>
        transformMeshBounds(const rdesc::Mesh& mesh, const ecs::Transform3D& transform) noexcept
        {
            const auto norm = transform.rotation.norm();
            const bool invalid_transform = !transform.translation.allFinite() || !transform.scale.allFinite() ||
                !transform.rotation.coeffs().allFinite() || !std::isfinite(norm) || norm <= 0;
            if (invalid_transform) return cxx::unexpected(error::Error{kInvalid});
            math::AABB local;
            if (mesh.bounds)
            {
                local = *mesh.bounds;
            }
            else
            {
                if (mesh.vertices.empty()) return cxx::unexpected(error::Error{kInvalid});
                local.min = local.max = mesh.vertices.front().position;
                for (const auto& vertex : mesh.vertices)
                {
                    if (!vertex.position.allFinite()) return cxx::unexpected(error::Error{kInvalid});
                    local.min = local.min.cwiseMin(vertex.position);
                    local.max = local.max.cwiseMax(vertex.position);
                }
            }
            if (!local.isValid() || !local.min.allFinite() || !local.max.allFinite())
                return cxx::unexpected(error::Error{kInvalid});
            const auto matrix = ecs::localTransformMatrix(transform);
            Eigen::Vector3d low = Eigen::Vector3d::Constant(INFINITY);
            Eigen::Vector3d high = Eigen::Vector3d::Constant(-INFINITY);
            for (unsigned corner = 0; corner < 8; ++corner)
            {
                const Eigen::Vector3d point{corner & 1 ? local.max.x() : local.min.x(),
                    corner & 2 ? local.max.y() : local.min.y(), corner & 4 ? local.max.z() : local.min.z()};
                const Eigen::Vector3d absolute = matrix * point;
                if (!absolute.allFinite()) return cxx::unexpected(error::Error{kInvalid});
                low = low.cwiseMin(absolute);
                high = high.cwiseMax(absolute);
            }
            return std::vector<spatial::Spatial3DQueryBounds>{
                {{low.x(), low.y(), low.z()}, {high.x(), high.y(), high.z()}}
            };
        }

        exec::task<cxx::expected<std::vector<spatial::Spatial3DQueryBounds>, error::Error>>
        captureMeshSceneBounds(EngineContext& context, const scene::SceneObjectData& object) noexcept
        {
            const auto main = context.execution().mainScheduler();
            const auto cpu = context.execution().cpuScheduler();
            auto fields = co_await stdexec::continues_on(stdexec::then(stdexec::schedule(cpu), [&]() noexcept
            {
                return decodeMeshSceneFields(object);
            }), main);
            if (!fields) co_return cxx::unexpected(fields.error());
            auto reference = context.assets().acquire<asset::MeshAsset>(fields->first.value.mesh);
            if (!reference) co_return cxx::unexpected(reference.error());
            auto mesh = co_await context.assets().read(*reference);
            if (!mesh) co_return cxx::unexpected(mesh.error());
            auto bounds = co_await stdexec::continues_on(stdexec::then(stdexec::schedule(cpu), [&]() noexcept
            {
                return transformMeshBounds(**mesh, fields->second);
            }), main);
            co_return bounds;
        }
    }

    bool registerMeshSceneContent(EngineMetaFunctionRegistry& metadata) noexcept
    {
        scene::GeometryDeclaration declaration;
        declaration.name = "lux.geometry.mesh3d";
        const auto add = [&](const auto& schemas, cxx::type_info type) noexcept
        {
            const auto schema = std::ranges::find(schemas, type, &ecs::ComponentSchema::cpp_type);
            if (schema == schemas.end()) return false;
            declaration.components.push_back({schema->id, schema->version, schema->cpp_type});
            return true;
        };
        if (!add(ecs::visualComponentSchemas(), cxx::make_basic_type_info<ecs::Mesh3D>()) ||
            !add(ecs::transformComponentSchemas(), cxx::make_basic_type_info<ecs::Transform3D>())) return false;
        declaration.visit_objects = &visitMeshSceneObjects;
        declaration.capture_bounds = &captureMeshSceneBounds;
        return metadata.addGeometry(std::move(declaration));
    }
} // namespace lux::engine::toolchain
