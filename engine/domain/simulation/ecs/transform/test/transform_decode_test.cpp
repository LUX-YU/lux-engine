#include <lux/engine/serialization/BinaryWriter.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace
{
    [[nodiscard]] const lux::simulation::ecs::ComponentSchema* findSchema(
        std::span<const lux::simulation::ecs::ComponentSchema> schemas,
        const lux::simulation::ecs::ComponentSchemaId& id
    ) noexcept
    {
        for (const auto& schema : schemas)
        {
            if (schema.id == id)
                return &schema;
        }
        return nullptr;
    }
}

int main()
{
    using namespace lux::simulation::ecs;

    const auto transforms = transformComponentSchemas();
    const auto* transform3d = findSchema(transforms, componentSchemaId("lux.ecs.Transform3D"));
    const auto* world3d = findSchema(transforms, componentSchemaId("lux.ecs.WorldTransform3D"));
    assert(transform3d != nullptr);
    assert(world3d != nullptr);
    assert(transform3d->decode_emplace != nullptr);
    assert(world3d->decode_emplace == nullptr);

    const auto hierarchy = hierarchyComponentSchemas();
    const auto* parent = findSchema(hierarchy, componentSchemaId("lux.ecs.Parent"));
    assert(parent != nullptr);
    assert(parent->decode_emplace == nullptr);

    Transform3D source{
        Eigen::Vector3d{1'000'000'000'000.125, -2.5, 3.75},
        Eigen::Quaterniond{Eigen::AngleAxisd{0.25, Eigen::Vector3d::UnitY()}},
        Eigen::Vector3d{1.0, 2.0, 3.0}
    };
    std::vector<std::byte> payload;
    lux::serialization::BinaryWriter writer(payload);
    const lux::serialization::SerializationBudget budget{payload.max_size(), payload.max_size(), 64U};
    assert(lux::serialization::write(writer, source, budget));

    Registry registry;
    const Entity entity = registry.create();
    assert(transform3d->decode_emplace(registry, entity, transform3d->version, payload));
    const auto& decoded = registry.get<const Transform3D>(entity);
    assert(decoded.translation.isApprox(source.translation, 0.000'000'001));
    assert(decoded.rotation.isApprox(source.rotation, 0.000'000'001));
    assert(decoded.scale.isApprox(source.scale, 0.000'000'001));

    const Entity unsupported_version_entity = registry.create();
    auto unsupported_version = transform3d->decode_emplace(
        registry,
        unsupported_version_entity,
        transform3d->version + 1U,
        payload
    );
    assert(!unsupported_version);
    assert(unsupported_version.error().code == EComponentDecodeError::UNSUPPORTED_VERSION);
    assert(!registry.all_of<Transform3D>(unsupported_version_entity));

    const Entity truncated_entity = registry.create();
    auto truncated = transform3d->decode_emplace(
        registry,
        truncated_entity,
        transform3d->version,
        std::span<const std::byte>(payload).first(payload.size() - 1U)
    );
    assert(!truncated);
    assert(!registry.all_of<Transform3D>(truncated_entity));

    auto trailing_payload = payload;
    trailing_payload.push_back(std::byte{});
    const Entity trailing_entity = registry.create();
    auto trailing = transform3d->decode_emplace(
        registry,
        trailing_entity,
        transform3d->version,
        trailing_payload
    );
    assert(!trailing);
    assert(trailing.error().code == EComponentDecodeError::MALFORMED_PAYLOAD);
    assert(!registry.all_of<Transform3D>(trailing_entity));

    const Entity destroyed = registry.create();
    registry.destroy(destroyed);
    auto invalid_entity = transform3d->decode_emplace(
        registry,
        destroyed,
        transform3d->version,
        payload
    );
    assert(!invalid_entity);
    assert(invalid_entity.error().code == EComponentDecodeError::INVALID_ENTITY);

    return 0;
}
