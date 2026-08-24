#pragma once

#include <lux/engine/ecs/ComponentCodec.hpp>
#include <lux/engine/ecs/ComponentOperations.hpp>
#include <lux/engine/ecs/ComponentSchemaId.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace lux::ecs
{
    enum class EComponentSnapshotPolicy : std::uint8_t
    {
        COPY,
        REBUILD,
    };

    struct ComponentSchema final
    {
        lux::cxx::TypeToken cpp_type;
        ComponentSchemaId id;
        std::uint32_t version{1};
        ComponentOperations operations;
        ComponentCodec codec;
        EComponentSnapshotPolicy snapshot{EComponentSnapshotPolicy::COPY};
        std::shared_ptr<const void> code_lifetime;
    };

    template <class Component>
    [[nodiscard]] ComponentSchema makeComponentSchema(
        ComponentSchemaId id,
        std::uint32_t version = 1,
        EComponentSnapshotPolicy snapshot = EComponentSnapshotPolicy::COPY,
        ComponentCodec codec = {},
        std::shared_ptr<const void> code_lifetime = {})
    {
        return ComponentSchema{
            lux::cxx::typeToken<Component>(),
            std::move(id),
            version,
            componentOperations<Component>(),
            codec,
            snapshot,
            std::move(code_lifetime)};
    }
} // namespace lux::ecs
