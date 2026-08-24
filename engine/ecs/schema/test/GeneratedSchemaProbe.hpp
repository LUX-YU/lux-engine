#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

namespace lux::ecs::test
{
    struct LUX_COMPONENT_SCHEMA("test.generated-probe", 7)
    GeneratedSchemaProbe final
    {
        std::int32_t LUX_MEMBER() value{};
    };
} // namespace lux::ecs::test
