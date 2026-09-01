#include <lux/engine/serialization/BinaryWriter.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>

#include <array>
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
            if (schema.id == id) return &schema;
        }
        return nullptr;
    }
}

int main()
{
    using namespace lux;
    using namespace lux::simulation::ecs;

    const auto schemas = visualComponentSchemas();
    const auto* mesh_schema = findSchema(schemas, componentSchemaId("lux.ecs.Mesh3D"));
    const auto* light_schema = findSchema(schemas, componentSchemaId("lux.ecs.Light3D"));
    assert(mesh_schema != nullptr && light_schema != nullptr);
    assert(mesh_schema->version == 2U && light_schema->version == 2U);
    assert(mesh_schema->decode_emplace != nullptr && light_schema->decode_emplace != nullptr);
    assert(mesh_schema->semantic_kind == EComponentSemanticKind::DOMAIN_CONTRACT && mesh_schema->editor_visible);
    assert(light_schema->semantic_kind == EComponentSemanticKind::DOMAIN_CONTRACT && light_schema->editor_visible);

    const std::array<std::uint8_t, 16> mesh_bytes{1U};
    const std::array<std::uint8_t, 16> material_bytes{2U};
    std::vector<std::byte> mesh_wire;
    serialization::BinaryWriter mesh_writer(mesh_wire);
    assert(mesh_writer.writeBytes(std::as_bytes(std::span(mesh_bytes))));
    assert(mesh_writer.writeBytes(std::as_bytes(std::span(material_bytes))));
    assert(mesh_writer.writeUnsigned<std::uint8_t>(1U));
    assert(mesh_writer.writeUnsigned<std::uint8_t>(1U));
    assert(mesh_writer.writeUnsigned<std::uint8_t>(0U));

    Registry registry;
    const Entity mesh_entity = registry.create();
    assert(mesh_schema->decode_emplace(registry, mesh_entity, 2U, mesh_wire));
    const auto& mesh = registry.get<const Mesh3D>(mesh_entity).value;
    assert(mesh.mesh == asset::AssetId{mesh_bytes});
    assert(mesh.material == asset::AssetId{material_bytes});
    assert(mesh.visible && mesh.cast_shadow && !mesh.receive_shadow);

    const Entity old_version_entity = registry.create();
    const auto old_version = mesh_schema->decode_emplace(registry, old_version_entity, 1U, mesh_wire);
    assert(!old_version && old_version.error().code == EComponentDecodeError::UNSUPPORTED_VERSION);

    const Entity truncated_entity = registry.create();
    const auto truncated = mesh_schema->decode_emplace(
        registry,
        truncated_entity,
        2U,
        std::span<const std::byte>{mesh_wire}.first(mesh_wire.size() - 1U)
    );
    assert(!truncated && !registry.all_of<Mesh3D>(truncated_entity));

    std::vector<std::byte> invalid_light_wire(98U, std::byte{});
    invalid_light_wire.front() = std::byte{0xffU};
    const Entity invalid_light_entity = registry.create();
    const auto invalid_light = light_schema->decode_emplace(registry, invalid_light_entity, 2U, invalid_light_wire);
    assert(!invalid_light && !registry.all_of<Light3D>(invalid_light_entity));
    return 0;
}
