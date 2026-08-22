#include <lux/engine/authoring/world/WorldAuthoringTransaction.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace lux::authoring
{
    namespace
    {
        [[nodiscard]] WorldAuthoringTransactionFailure failure(
            EWorldAuthoringTransactionError error,
            std::string detail,
            bool can_convert = false)
        {
            return {error, std::move(detail), can_convert};
        }

        [[nodiscard]] bool sameCell(
            const WorldSourceDocument& root,
            const WorldInstancePageDocument& page,
            const WorldActorSourcePosition& position)
        {
            const auto space = std::ranges::find(
                root.spaces, page.space, &lux::authoring::PartitionSpaceDescriptor::id);
            if (space == root.spaces.end() || page.cell.topology != space->topology)
                return false;
            const auto coordinate = [&](double value)
                -> std::optional<std::int64_t>
            {
                if (!std::isfinite(value) ||
                    !lux::authoring::isValidCellEdge(space->cell_edge))
                    return std::nullopt;
                const auto result = std::floor(
                    value / static_cast<double>(space->cell_edge));
                if (result < static_cast<double>(
                        std::numeric_limits<std::int64_t>::min()) ||
                    result > static_cast<double>(
                        std::numeric_limits<std::int64_t>::max()))
                    return std::nullopt;
                return static_cast<std::int64_t>(result);
            };
            if (space->topology == lux::authoring::EPartitionTopology::PLANAR_XY)
            {
                const auto* point = std::get_if<lux::math::Position2d>(
                    &position);
                const auto x = point ? coordinate(point->x) : std::nullopt;
                const auto y = point ? coordinate(point->y) : std::nullopt;
                const auto* expected = std::get_if<lux::authoring::PlanarCellCoord>(
                    &page.cell.coordinate);
                return x && y && expected &&
                    lux::authoring::PlanarCellCoord{*x, *y} == *expected;
            }
            const auto* point = std::get_if<lux::math::Position3d>(&position);
            if (!point)
                return false;
            if (space->topology == lux::authoring::EPartitionTopology::PLANAR_XZ)
            {
                const auto x = coordinate(point->x);
                const auto z = coordinate(point->z);
                const auto* expected = std::get_if<lux::authoring::PlanarCellCoord>(
                    &page.cell.coordinate);
                return x && z && expected &&
                    lux::authoring::PlanarCellCoord{*x, *z} == *expected;
            }
            const auto x = coordinate(point->x);
            const auto y = coordinate(point->y);
            const auto z = coordinate(point->z);
            const auto* expected = std::get_if<lux::authoring::VolumeCellCoord>(
                &page.cell.coordinate);
            return x && y && z && expected &&
                lux::authoring::VolumeCellCoord{*x, *y, *z} == *expected;
        }

        void appendTombstone(
            WorldInstancePageDocument& page,
            std::uint64_t local_id)
        {
            std::erase(page.tombstones, local_id);
            page.tombstones.push_back(local_id);
        }

        void removeTombstone(
            WorldInstancePageDocument& page,
            std::uint64_t local_id)
        {
            std::erase(page.tombstones, local_id);
        }

        [[nodiscard]] bool samePage(
            const WorldInstancePageDocument& left,
            const WorldInstancePageDocument& right) noexcept
        {
            return left.world == right.world &&
                left.instance_set == right.instance_set &&
                left.space == right.space && left.cell == right.cell;
        }

        [[nodiscard]] const WorldInstanceSetSourceDescriptor* findInstanceSet(
            const WorldSourceDocument& root,
            lux::authoring::InstanceSetId id) noexcept
        {
            const auto found = std::ranges::find(
                root.instance_sets,
                id,
                &WorldInstanceSetSourceDescriptor::id);
            return found == root.instance_sets.end() ? nullptr : &*found;
        }
    } // namespace

    lux::cxx::expected<
        InstanceToActorTransaction,
        WorldAuthoringTransactionFailure>
    convertInstanceToActor(
        const WorldSourceDocument& root,
        const WorldInstancePageDocument& source_instance_page,
        const WorldDescriptorPageDocument& source_actor_page,
        lux::authoring::WorldInstanceId instance_id,
        WorldActorSourceDescriptor descriptor,
        WorldActorDocument document)
    {
        if (root.world.empty() || source_instance_page.world != root.world
            || source_actor_page.world != root.world
            || instance_id.set != source_instance_page.instance_set
            || descriptor.id.empty() || document.actor != descriptor.id
            || document.world != root.world || descriptor.space !=
                source_actor_page.space)
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::INVALID_ARGUMENT,
                "Instance-to-Actor transaction has inconsistent identities"));
        }
        const auto found = std::ranges::find(
            source_instance_page.instances,
            instance_id,
            &EditableWorldInstance::id);
        if (found == source_instance_page.instances.end())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::OBJECT_NOT_FOUND,
                "Instance is not present in its source Page"));
        }
        if (std::ranges::find(
                source_actor_page.actors,
                descriptor.id,
                &WorldActorSourceDescriptor::id) != source_actor_page.actors.end())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::IDENTITY_CONFLICT,
                "destination Descriptor Page already contains Actor"));
        }
        descriptor.position = found->position;
        descriptor.transform_parent.reset();
        descriptor.data_layers = found->data_layers;
        document.actor_class = descriptor.actor_class;
        document.space = descriptor.space;
        document.position = descriptor.position;
        document.transform_parent.reset();
        document.data_layers = descriptor.data_layers;
        document.references = descriptor.references;

        InstanceToActorTransaction result{
            source_instance_page,
            source_actor_page,
            std::move(descriptor),
            std::move(document)};
        std::erase_if(
            result.instance_page.instances,
            [instance_id](const auto& instance)
            {
                return instance.id == instance_id;
            });
        appendTombstone(result.instance_page, instance_id.local_id);
        result.actor_descriptor_page.actors.push_back(result.actor_descriptor);
        return result;
    }

    lux::cxx::expected<
        ActorToInstanceTransaction,
        WorldAuthoringTransactionFailure>
    convertActorToInstance(
        const WorldSourceDocument& root,
        const WorldDescriptorPageDocument& source_actor_page,
        const WorldActorDocument& document,
        const WorldInstancePageDocument& source_instance_page,
        EditableWorldInstance instance,
        std::span<const std::string_view> allowed_component_schemas)
    {
        const auto descriptor = std::ranges::find(
            source_actor_page.actors,
            document.actor,
            &WorldActorSourceDescriptor::id);
        const auto* allocator = findInstanceSet(
            root, source_instance_page.instance_set);
        if (document.world != root.world || source_actor_page.world != root.world
            || source_instance_page.world != root.world
            || descriptor == source_actor_page.actors.end()
            || !allocator || document.actor_class != descriptor->actor_class
             || document.space != descriptor->space
             || document.position != descriptor->position
             || document.transform_parent != descriptor->transform_parent
             || document.transform_parent.has_value()
            || document.data_layers != descriptor->data_layers
            || document.references != descriptor->references)
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::INVALID_ARGUMENT,
                "Actor-to-Instance transaction has inconsistent identities"));
        }
        for (const auto& component : document.components)
        {
            if (std::ranges::find(
                    allowed_component_schemas,
                    component.schema_name) == allowed_component_schemas.end())
            {
                return lux::cxx::unexpected(failure(
                    EWorldAuthoringTransactionError::
                        UNSUPPORTED_INSTANCE_COMPONENT,
                    "Actor component '" + component.schema_name
                        + "' is outside the Instance allow-list",
                    true));
            }
        }
        instance.id = {
            source_instance_page.instance_set,
            allocator->next_local_id};
        instance.position = descriptor->position;
        instance.data_layers = descriptor->data_layers;
        if (!sameCell(root, source_instance_page, instance.position))
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::DESTINATION_MISMATCH,
                "Actor position is outside the destination Instance Cell"));
        }

        ActorToInstanceTransaction result{
            source_actor_page,
            source_instance_page,
            *allocator,
            instance,
            document.actor};
        std::erase(
        result.actor_descriptor_page.actors, *descriptor);
        result.instance_page.instances.push_back(result.instance);
        if (result.instance_set.next_local_id ==
            std::numeric_limits<std::uint64_t>::max())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::IDENTITY_EXHAUSTED,
                "Instance Set local id allocator is exhausted"));
        }
        ++result.instance_set.next_local_id;
        return result;
    }

    lux::cxx::expected<void, WorldAuthoringTransactionFailure>
    validateInstanceComponentAddition(std::string_view component_schema)
    {
        if (component_schema.empty())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::INVALID_ARGUMENT,
                "component schema name is empty"));
        }
        return lux::cxx::unexpected(failure(
            EWorldAuthoringTransactionError::UNSUPPORTED_INSTANCE_COMPONENT,
            "Instances have a closed schema; use Convert Instance to Actor",
            true));
    }


    lux::cxx::expected<
        WorldInstanceDeleteTransaction,
        WorldAuthoringTransactionFailure>
    deleteWorldInstance(
        const WorldInstancePageDocument& source_page,
        lux::authoring::WorldInstanceId instance_id)
    {
        if (!instance_id.valid() ||
            instance_id.set != source_page.instance_set)
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::INVALID_ARGUMENT,
                "Instance delete has inconsistent identity"));
        }
        const auto found = std::ranges::find(
            source_page.instances,
            instance_id,
            &EditableWorldInstance::id);
        if (found == source_page.instances.end())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::OBJECT_NOT_FOUND,
                "Instance is not present in its source Page"));
        }

        WorldInstanceDeleteTransaction result{source_page, *found};
        std::erase(result.page.instances, *found);
        appendTombstone(result.page, instance_id.local_id);
        return result;
    }


    lux::cxx::expected<
        WorldInstanceDuplicateTransaction,
        WorldAuthoringTransactionFailure>
    duplicateWorldInstance(
        const WorldSourceDocument& root,
        const WorldInstancePageDocument& source_page,
        lux::authoring::WorldInstanceId instance_id,
        WorldActorSourcePosition position)
    {
        if (source_page.world != root.world || !instance_id.valid() ||
            instance_id.set != source_page.instance_set)
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::INVALID_ARGUMENT,
                "Instance duplicate has inconsistent identity"));
        }
        const auto* allocator = findInstanceSet(root, source_page.instance_set);
        if (!allocator)
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::INVALID_ARGUMENT,
                "Instance Set allocator is absent from LXWA"));
        }
        if (allocator->next_local_id ==
            std::numeric_limits<std::uint64_t>::max())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::IDENTITY_EXHAUSTED,
                "Instance Set local id allocator is exhausted"));
        }
        const auto found = std::ranges::find(
            source_page.instances,
            instance_id,
            &EditableWorldInstance::id);
        if (found == source_page.instances.end())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::OBJECT_NOT_FOUND,
                "Instance is not present in its source Page"));
        }
        if (!sameCell(root, source_page, position))
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::DESTINATION_MISMATCH,
                "Duplicated Instance position is outside its Page Cell"));
        }

        WorldInstanceDuplicateTransaction result{
            source_page, *allocator, *found};
        result.created_instance.id = {
            source_page.instance_set,
            allocator->next_local_id};
        result.created_instance.position = std::move(position);
        removeTombstone(
            result.page, result.created_instance.id.local_id);
        result.page.instances.push_back(result.created_instance);
        ++result.instance_set.next_local_id;
        return result;
    }


    lux::cxx::expected<
        WorldInstanceMoveTransaction,
        WorldAuthoringTransactionFailure>
    moveWorldInstance(
        const WorldSourceDocument& root,
        const WorldInstancePageDocument& source,
        const WorldInstancePageDocument& destination,
        lux::authoring::WorldInstanceId instance_id,
        WorldActorSourcePosition position)
    {
        if (source.world != root.world || destination.world != root.world ||
            !instance_id.valid() || instance_id.set != source.instance_set ||
            destination.instance_set != source.instance_set ||
            destination.space != source.space)
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::INVALID_ARGUMENT,
                "Instance move has inconsistent World, Set, or Space"));
        }
        const auto found = std::ranges::find(
            source.instances,
            instance_id,
            &EditableWorldInstance::id);
        if (found == source.instances.end())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::OBJECT_NOT_FOUND,
                "Instance is not present in its source Page"));
        }
        if (!sameCell(root, destination, position))
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::DESTINATION_MISMATCH,
                "Moved Instance position is outside the destination Cell"));
        }

        WorldInstanceMoveTransaction result;
        result.source_page = source;
        result.moved_instance = *found;
        result.moved_instance.position = std::move(position);
        if (samePage(source, destination))
        {
            const auto mutable_instance = std::ranges::find(
                result.source_page.instances,
                instance_id,
                &EditableWorldInstance::id);
            *mutable_instance = result.moved_instance;
            return result;
        }
        if (std::ranges::find(
                destination.instances,
                instance_id,
                &EditableWorldInstance::id) != destination.instances.end())
        {
            return lux::cxx::unexpected(failure(
                EWorldAuthoringTransactionError::IDENTITY_CONFLICT,
                "destination Page already contains Instance"));
        }

        std::erase(result.source_page.instances, *found);
        appendTombstone(result.source_page, instance_id.local_id);
        result.destination_page = destination;
        removeTombstone(*result.destination_page, instance_id.local_id);
        result.destination_page->instances.push_back(result.moved_instance);
        return result;
    }
} // namespace lux::authoring
