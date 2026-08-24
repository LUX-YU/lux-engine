#include <lux/engine/ecs/HierarchySchema.hpp>

#include <lux/engine/ecs/Parent.hpp>

#include <array>

namespace lux::ecs
{
    namespace
    {
        lux::cxx::expected<void, EComponentCodecError> encodeParent(
            const ComponentSchema&,
            const World& world,
            Entity entity,
            ComponentEncodePort& port
        ) noexcept
        {
            return port.writeEntity("entity", world.get<Parent>(entity).entity);
        }

        lux::cxx::expected<void, EComponentCodecError> decodeParent(
            const ComponentSchema&,
            WorldEdit& edit,
            Entity entity,
            std::uint32_t version,
            ComponentDecodePort& port
        ) noexcept
        {
            if (version != 1)
                return lux::cxx::unexpected(EComponentCodecError::UNSUPPORTED_VERSION);
            Parent value;
            EncodedPropertyView property;
            while (port.next(property))
            {
                if (property.name == "entity")
                {
                    auto resolved = port.resolveEntity(property.bytes);
                    if (!resolved)
                        return lux::cxx::unexpected(resolved.error());
                    value.entity = *resolved;
                }
            }
            if (value.entity == NullEntity)
                return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
            edit.emplace<Parent>(entity, value);
            return {};
        }
    }

    std::span<const ComponentSchema> hierarchyComponentSchemas()
    {
        static const std::array schemas{
            makeComponentSchema<Parent>(
                componentSchemaId("lux.ecs.Parent"),
                1,
                ComponentSnapshotMode::Copy,
                ComponentCodec{&encodeParent, &decodeParent}
            )};
        return schemas;
    }
} // namespace lux::ecs
