#pragma once

#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/WorldResidency.hpp>
#include <lux/engine/serialization/SerializationError.hpp>

#include <optional>
#include <variant>

namespace lux::editor
{
    struct SceneComponentData final
    {
        simulation::ecs::ComponentSchemaId schema;
        std::uint32_t version{};
        std::vector<std::byte> payload;
    };

    struct SceneObjectData final
    {
        world::WorldObjectId id;
        partition::PartitionOrdinal partition;
        std::vector<SceneComponentData> components;
    };

    struct SceneComponentEdit final
    {
        world::WorldObjectId object;
        simulation::ecs::ComponentSchemaId schema;
        // No value means remove the component. A value is the complete validated author component.
        std::optional<SceneComponentData> value;
    };

    struct SceneCreateObjects final
    {
        std::vector<SceneObjectData> objects;
    };

    struct SceneEraseObjects final
    {
        std::vector<world::WorldObjectId> objects;
    };

    struct SceneWriteComponents final
    {
        std::vector<SceneComponentEdit> components;
    };

    using VSceneEdit = std::variant<SceneCreateObjects, SceneEraseObjects, SceneWriteComponents>;

    struct SceneEdit final
    {
        std::uint64_t based_on{};
        VSceneEdit operation;
    };

    enum class ESceneEditError : std::uint8_t
    {
        STALE_REVISION,
        READ_ONLY,
        INVALID_OBJECT,
        INVALID_COMPONENT,
        DUPLICATE_WRITE,
        HIERARCHY_CYCLE,
        UNKNOWN_PAYLOAD,
        NO_HISTORY,
        NOT_RESIDENT
    };

    using VSceneEditFailure = std::variant<
        ESceneEditError,
        scene::SceneRuntimeFailure,
        scene::WorldResidencyFailure,
        simulation::ecs::ComponentDecodeFailure,
        serialization::SerializationFailure,
        world::WorldStorageCodecFailure,
        scene::ScenePackageFailure>;
    template <class T> using SceneEditResult = cxx::expected<T, VSceneEditFailure>;
} // namespace lux::editor
