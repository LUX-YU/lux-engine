#pragma once

#include <lux/engine/ecs/ComponentCodec.hpp>
#include <lux/engine/ecs/ComponentOperations.hpp>
#include <lux/engine/ecs/ComponentSchemaId.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace lux::meta
{
    struct RefClass;
}

namespace lux::ecs
{
    enum class ComponentSnapshotMode : std::uint8_t
    {
        Copy,
        Rebuild,
    };

    struct ComponentSchema final
    {
        lux::cxx::TypeToken cpp_type;
        ComponentSchemaId id;
        std::uint32_t version{1};
        const lux::meta::RefClass* reflection{};
        ComponentOperations operations;
        ComponentCodec codec;
        ComponentSnapshotMode snapshot{ComponentSnapshotMode::Copy};
        std::shared_ptr<const void> code_lifetime;
    };

    template <class Component>
    [[nodiscard]] ComponentSchema makeComponentSchema(
        ComponentSchemaId id,
        std::uint32_t version = 1,
        ComponentSnapshotMode snapshot = ComponentSnapshotMode::Copy,
        ComponentCodec codec = {},
        const lux::meta::RefClass* reflection = nullptr,
        std::shared_ptr<const void> code_lifetime = {})
    {
        return ComponentSchema{
            lux::cxx::typeToken<Component>(),
            std::move(id),
            version,
            reflection,
            componentOperations<Component>(),
            codec,
            snapshot,
            std::move(code_lifetime)};
    }
} // namespace lux::ecs
