#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

namespace lux::ecs::test
{
    struct LUX_COMPONENT_SCHEMA("test.generated-probe", 7)
    GeneratedSchemaProbe final
    {
        std::int32_t LUX_MEMBER() value{};
    };

    struct LUX_COMPONENT() GeneratedDefaultComponent final
    {
        std::int32_t LUX_MEMBER() value{};
    };

    struct LUX_META(
        luxref::class,
        component=true,
        schema_name="test.generated-rebuild-reflected",
        schema_version=3,
        snapshot=rebuild,
        codec=reflected
    ) GeneratedRebuildReflected final
    {
        std::int32_t LUX_MEMBER() value{};
    };

    struct LUX_REBUILD_COMPONENT_SCHEMA("test.generated-rebuild-none", 2)
    GeneratedRebuildNone final
    {
        std::int32_t LUX_MEMBER() value{};
    };
} // namespace lux::ecs::test
