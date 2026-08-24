#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>

namespace lux::ecs
{
    template <class Component>
    [[nodiscard]] ComponentSchema makeGeneratedComponentSchema(
        const lux::meta::RefClass& reflection,
        std::string_view stable_name,
        std::uint32_t version,
        ComponentSnapshotMode snapshot)
    {
        return makeComponentSchema<Component>(
            componentSchemaId(stable_name),
            version,
            snapshot,
            snapshot == ComponentSnapshotMode::Copy
                ? reflectedComponentCodec()
                : ComponentCodec{},
            &reflection
        );
    }
} // namespace lux::ecs
