#pragma once

#include <lux/engine/authoring/world/visibility.h>
#include <lux/engine/authoring/world/WorldSource.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <span>
#include <string>
#include <string_view>
#include <optional>

namespace lux::authoring
{
    enum class EWorldAuthoringTransactionError : std::uint8_t
    {
        INVALID_ARGUMENT,
        OBJECT_NOT_FOUND,
        IDENTITY_CONFLICT,
        IDENTITY_EXHAUSTED,
        UNSUPPORTED_INSTANCE_COMPONENT,
        DESTINATION_MISMATCH
    };

    struct WorldAuthoringTransactionFailure final
    {
        EWorldAuthoringTransactionError error{
            EWorldAuthoringTransactionError::INVALID_ARGUMENT};
        std::string detail;
        bool can_convert_to_actor{false};
    };

    struct InstanceToActorTransaction final
    {
        WorldInstancePageDocument instance_page;
        WorldDescriptorPageDocument actor_descriptor_page;
        WorldActorSourceDescriptor actor_descriptor;
        WorldActorDocument actor_document;
    };

    /// Explicitly materializes one Instance as an Actor. Removal, tombstone,
    /// descriptor insertion and the new LXAD are returned as one immutable
    /// transaction result; the Editor writes children and commits LXWA last.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        InstanceToActorTransaction,
        WorldAuthoringTransactionFailure>
    convertInstanceToActor(
        const WorldSourceDocument& root,
        const WorldInstancePageDocument& instance_page,
        const WorldDescriptorPageDocument& actor_descriptor_page,
        lux::authoring::WorldInstanceId instance,
        WorldActorSourceDescriptor actor_descriptor,
        WorldActorDocument actor_document);

    struct ActorToInstanceTransaction final
    {
        WorldDescriptorPageDocument actor_descriptor_page;
        WorldInstancePageDocument instance_page;
        WorldInstanceSetSourceDescriptor instance_set;
        EditableWorldInstance instance;
        lux::authoring::WorldActorId removed_actor;
    };

    /// Converts an Actor only after every reflected component is admitted by
    /// the caller's explicit allow-list. Opaque component payload is never
    /// silently discarded.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        ActorToInstanceTransaction,
        WorldAuthoringTransactionFailure>
    convertActorToInstance(
        const WorldSourceDocument& root,
        const WorldDescriptorPageDocument& actor_descriptor_page,
        const WorldActorDocument& actor_document,
        const WorldInstancePageDocument& instance_page,
        EditableWorldInstance instance,
        std::span<const std::string_view> allowed_component_schemas);

    /// Instance is a closed schema. This helper is the validation path used by
    /// Add Component UI and always returns an executable conversion hint for
    /// non-empty component requests.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        void,
        WorldAuthoringTransactionFailure>
    validateInstanceComponentAddition(std::string_view component_schema);

    struct WorldInstanceDeleteTransaction final
    {
        WorldInstancePageDocument page;
        EditableWorldInstance removed_instance;
    };

    /// Deletes one Instance without reusing its local id. The returned Page
    /// contains a single tombstone for the removed id even when malformed
    /// input already contained duplicate tombstones.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        WorldInstanceDeleteTransaction,
        WorldAuthoringTransactionFailure>
    deleteWorldInstance(
        const WorldInstancePageDocument& page,
        lux::authoring::WorldInstanceId instance);

    struct WorldInstanceDuplicateTransaction final
    {
        WorldInstancePageDocument page;
        WorldInstanceSetSourceDescriptor instance_set;
        EditableWorldInstance created_instance;
    };

    /// Duplicates within the same Instance Page. The LXWA Instance Set
    /// allocator is advanced only in the returned transaction, so allocation
    /// remains unique after the Set spans multiple Cell pages.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        WorldInstanceDuplicateTransaction,
        WorldAuthoringTransactionFailure>
    duplicateWorldInstance(
        const WorldSourceDocument& root,
        const WorldInstancePageDocument& page,
        lux::authoring::WorldInstanceId instance,
        WorldActorSourcePosition position);

    struct WorldInstanceMoveTransaction final
    {
        WorldInstancePageDocument source_page;
        std::optional<WorldInstancePageDocument> destination_page;
        EditableWorldInstance moved_instance;

        [[nodiscard]] bool crossedCell() const noexcept
        {
            return destination_page.has_value();
        }
    };

    /// Moves an Instance while preserving its stable id. When source and
    /// destination identify the same Page only `source_page` is returned.
    /// Cross-Cell moves atomically return both replacement Pages: the source
    /// gains a tombstone and the destination removes a stale tombstone before
    /// adopting the Instance.
    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC lux::cxx::expected<
        WorldInstanceMoveTransaction,
        WorldAuthoringTransactionFailure>
    moveWorldInstance(
        const WorldSourceDocument& root,
        const WorldInstancePageDocument& source_page,
        const WorldInstancePageDocument& destination_page,
        lux::authoring::WorldInstanceId instance,
        WorldActorSourcePosition position);
} // namespace lux::authoring
