#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/schema_reflection/visibility.h>

#include <cstdint>
#include <string_view>

namespace lux::meta
{
    struct RefClass;
}

namespace lux::ecs
{
    enum class EGeneratedComponentCodec : std::uint8_t
    {
        NONE,
        REFLECTED,
    };

    [[nodiscard]] LUX_ENGINE_ECS_SCHEMA_REFLECTION_PUBLIC ComponentCodec
    reflectedComponentCodec(const lux::meta::RefClass& reflection) noexcept;

    template <class Component>
    [[nodiscard]] ComponentSchema makeGeneratedComponentSchema(
        const lux::meta::RefClass& reflection,
        std::string_view stable_name,
        std::uint32_t version,
        EComponentSnapshotPolicy snapshot,
        EGeneratedComponentCodec codec)
    {
        return makeComponentSchema<Component>(
            componentSchemaId(stable_name),
            version,
            snapshot,
            codec == EGeneratedComponentCodec::REFLECTED
                ? reflectedComponentCodec(reflection)
                : ComponentCodec{}
        );
    }
} // namespace lux::ecs
