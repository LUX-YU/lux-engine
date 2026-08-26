#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldPartitioner.hpp>
#include <lux/engine/world/detail/WorldFailureInjection.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::world;

    [[nodiscard]] WorldObjectId objectId(std::uint64_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        for (std::size_t index{}; index < sizeof(value); ++index)
        {
            bytes[15U - index] = static_cast<std::uint8_t>(
                value >> (index * 8U)
            );
        }
        return WorldObjectId{uuids::uuid(bytes)};
    }

    [[nodiscard]] WorldPartitionId partitionId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x80U;
        bytes[15] = tail;
        return WorldPartitionId{uuids::uuid(bytes)};
    }

    class SingleWorkspace final : public WorldPartitionWorkspace
    {
      public:
        SingleWorkspace()
            : descriptor_{worldPartitionerId("test.single"), 1U}
        {
        }

        const WorldPartitionerDescriptor& descriptor() const noexcept override
        {
            return descriptor_;
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        doRebuild(const WorldDescription&) noexcept override
        {
            return {};
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        doObjectAdded(WorldObjectSnapshotView) noexcept override
        {
            return {};
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        doObjectChanged(WorldObjectSnapshotView) noexcept override
        {
            return {};
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        doObjectRemoved(WorldObjectId) noexcept override
        {
            return {};
        }

        lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>
        doFreeze(const WorldDescription& world) const noexcept override
        {
            WorldPartitionLayoutBuilder builder(world);
            std::vector<WorldObjectId> objects;
            try
            {
                objects.reserve(world.objectCount());
                for (std::size_t index{}; index < world.objectCount(); ++index)
                    objects.push_back(world.objectAt(index).id());
            }
            catch (...)
            {
                WorldPartitionFailure failure;
                failure.code = EWorldPartitionError::ALLOCATION_FAILURE;
                return lux::cxx::unexpected(std::move(failure));
            }
            if (!objects.empty())
            {
                auto added = builder.addPartition(partitionId(1U), objects);
                if (!added)
                    return lux::cxx::unexpected(added.error());
            }
            auto layout = std::move(builder).build();
            if (!layout)
                return lux::cxx::unexpected(layout.error());
            return WorldPartitionBuildProduct::build(
                descriptor_,
                std::move(*layout),
                {}
            );
        }

      private:
        WorldPartitionerDescriptor descriptor_;
    };

    class SinglePartitioner final : public WorldPartitioner
    {
      public:
        WorldPartitionerDescriptor descriptor() const noexcept override
        {
            return {worldPartitionerId("test.single"), 1U};
        }

        lux::cxx::expected<
            std::unique_ptr<WorldPartitionWorkspace>,
            WorldPartitionFailure>
        createWorkspaceImplementation() const noexcept override
        {
            try
            {
                return std::unique_ptr<WorldPartitionWorkspace>(
                    std::make_unique<SingleWorkspace>()
                );
            }
            catch (...)
            {
                WorldPartitionFailure failure;
                failure.code = EWorldPartitionError::ALLOCATION_FAILURE;
                return lux::cxx::unexpected(std::move(failure));
            }
        }
    };

    [[nodiscard]] std::uint8_t quadrant(WorldObjectSnapshotView object)
    {
        assert(object.valid());
        assert(!object.data.empty());
        assert(object.data.front().payload.size() == 2U);
        const auto x = std::to_integer<std::uint8_t>(object.data.front().payload[0]);
        const auto y = std::to_integer<std::uint8_t>(object.data.front().payload[1]);
        return static_cast<std::uint8_t>((x >= 128U ? 1U : 0U) |
                                         (y >= 128U ? 2U : 0U));
    }

    class QuadtreeWorkspace final : public WorldPartitionWorkspace
    {
      public:
        QuadtreeWorkspace()
            : descriptor_{worldPartitionerId("test.quadtree2d"), 1U}
        {
        }

        void failNextIncrementalForTest() noexcept
        {
            fail_next_incremental_ = true;
        }

        const WorldPartitionerDescriptor& descriptor() const noexcept override
        {
            return descriptor_;
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        doRebuild(const WorldDescription& world) noexcept override
        {
            try
            {
                objects_.clear();
                for (std::size_t index{}; index < world.objectCount(); ++index)
                {
                    const auto object = world.objectAt(index);
                    const auto data = object.dataAt(0U);
                    const WorldDataSnapshotView snapshot_data{
                        &data.schema(),
                        data.version(),
                        data.payload()};
                    const WorldObjectSnapshotView snapshot{
                        object.id(),
                        std::span(&snapshot_data, 1U)};
                    objects_.emplace(object.id(), quadrant(snapshot));
                }
                return {};
            }
            catch (...)
            {
                WorldPartitionFailure failure;
                failure.code = EWorldPartitionError::ALLOCATION_FAILURE;
                return lux::cxx::unexpected(std::move(failure));
            }
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        doObjectAdded(WorldObjectSnapshotView object) noexcept override
        {
            return assign(object);
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        doObjectChanged(WorldObjectSnapshotView object) noexcept override
        {
            if (fail_next_incremental_)
            {
                fail_next_incremental_ = false;
                WorldPartitionFailure failure;
                failure.code = EWorldPartitionError::IMPLEMENTATION_FAILURE;
                return lux::cxx::unexpected(std::move(failure));
            }
            return assign(object);
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        doObjectRemoved(WorldObjectId object) noexcept override
        {
            objects_.erase(object);
            return {};
        }

        lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>
        doFreeze(const WorldDescription& world) const noexcept override
        {
            try
            {
                std::array<std::vector<WorldObjectId>, 4> groups;
                for (std::size_t index{}; index < world.objectCount(); ++index)
                {
                    const auto id = world.objectAt(index).id();
                    const auto found = objects_.find(id);
                    if (found == objects_.end())
                    {
                        WorldPartitionFailure failure;
                        failure.code = EWorldPartitionError::IMPLEMENTATION_FAILURE;
                        failure.object = id;
                        return lux::cxx::unexpected(std::move(failure));
                    }
                    groups[found->second].push_back(id);
                }

                WorldPartitionLayoutBuilder builder(world);
                for (std::size_t index{}; index < groups.size(); ++index)
                {
                    if (groups[index].empty())
                        continue;
                    auto added = builder.addPartition(
                        partitionId(static_cast<std::uint8_t>(index + 1U)),
                        groups[index]
                    );
                    if (!added)
                        return lux::cxx::unexpected(added.error());
                }
                auto layout = std::move(builder).build();
                if (!layout)
                    return lux::cxx::unexpected(layout.error());
                return WorldPartitionBuildProduct::build(
                    descriptor_,
                    std::move(*layout),
                    {{worldPartitionIndexTypeId("test.quadtree.ordinal"), 1U, {}}}
                );
            }
            catch (...)
            {
                WorldPartitionFailure failure;
                failure.code = EWorldPartitionError::ALLOCATION_FAILURE;
                return lux::cxx::unexpected(std::move(failure));
            }
        }

      private:
        lux::cxx::expected<void, WorldPartitionFailure>
        assign(WorldObjectSnapshotView object) noexcept
        {
            if (!object.valid() || object.data.empty())
            {
                WorldPartitionFailure failure;
                failure.code = EWorldPartitionError::IMPLEMENTATION_FAILURE;
                failure.object = object.id;
                return lux::cxx::unexpected(std::move(failure));
            }
            try
            {
                objects_[object.id] = quadrant(object);
                return {};
            }
            catch (...)
            {
                WorldPartitionFailure failure;
                failure.code = EWorldPartitionError::ALLOCATION_FAILURE;
                return lux::cxx::unexpected(std::move(failure));
            }
        }

        WorldPartitionerDescriptor descriptor_;
        std::unordered_map<WorldObjectId, std::uint8_t, WorldObjectIdHash> objects_;
        bool fail_next_incremental_{};
    };

    [[nodiscard]] WorldDescription makeWorld()
    {
        WorldDescriptionBuilder builder;
        const auto transform = worldDataSchemaId("test.position2d");
        for (std::uint8_t index = 1U; index <= 4U; ++index)
        {
            const auto object = objectId(index);
            assert(builder.addObject(object));
            const std::array payload{
                std::byte{static_cast<std::uint8_t>(index * 64U - 1U)},
                std::byte{static_cast<std::uint8_t>((index & 1U) * 192U)}};
            assert(builder.addData(object, transform, 1U, payload));
        }
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }
} // namespace

int main()
{
    static_assert(!std::is_default_constructible_v<WorldPartitionLayout>);
    static_assert(!std::is_default_constructible_v<WorldPartitionBuildProduct>);

    const auto hashed_object = objectId(17U);
    assert(
        WorldObjectIdHash{}(hashed_object) ==
        std::hash<uuids::uuid>{}(hashed_object.value)
    );
    const auto hashed_partition = partitionId(17U);
    assert(
        WorldPartitionIdHash{}(hashed_partition) ==
        std::hash<uuids::uuid>{}(hashed_partition.value)
    );

    using namespace lux::world;

    WorldDescriptionBuilder invalid;
    assert(!invalid.addObject({}));
    assert(invalid.addObject(objectId(1U)));
    assert(!invalid.addObject(objectId(1U)));
    assert(!invalid.addData(
        objectId(1U),
        {},
        1U,
        {}
    ));
    assert(!invalid.addData(
        objectId(1U),
        worldDataSchemaId("test.data"),
        0U,
        {}
    ));

    WorldDescriptionBuilder failure_builder;
    detail::failNextWorldOperationForTest(
        detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION
    );
    const auto failed_object = failure_builder.addObject(objectId(8U));
    assert(!failed_object);
    assert(
        failed_object.error().code ==
        EWorldDescriptionError::ALLOCATION_FAILURE
    );
    assert(failure_builder.addObject(objectId(8U)));

    const std::array failure_payload{std::byte{8U}};
    detail::failNextWorldOperationForTest(
        detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION
    );
    const auto failed_data = failure_builder.addData(
        objectId(8U),
        worldDataSchemaId("test.failure"),
        1U,
        failure_payload
    );
    assert(!failed_data);
    assert(
        failed_data.error().code ==
        EWorldDescriptionError::ALLOCATION_FAILURE
    );
    assert(failure_builder.addData(
        objectId(8U),
        worldDataSchemaId("test.failure"),
        1U,
        failure_payload
    ));

    detail::failNextWorldOperationForTest(
        detail::EWorldFailurePoint::DESCRIPTION_BUILD_ALLOCATION
    );
    const auto failed_description_build = std::move(failure_builder).build();
    assert(!failed_description_build);
    assert(
        failed_description_build.error().code ==
        EWorldDescriptionError::ALLOCATION_FAILURE
    );
    detail::failNextWorldOperationForTest(
        detail::EWorldFailurePoint::DESCRIPTION_BUILD_SIZE_OVERFLOW
    );
    const auto overflow_description_build = std::move(failure_builder).build();
    assert(!overflow_description_build);
    assert(
        overflow_description_build.error().code ==
        EWorldDescriptionError::SIZE_OVERFLOW
    );
    auto failure_world = std::move(failure_builder).build();
    assert(failure_world && failure_world->objectCount() == 1U);

    constexpr std::size_t shared_schema_object_count = 10'000U;
    WorldDescriptionBuilder shared_schema_builder;
    const auto shared_schema = worldDataSchemaId("test.shared");
    for (std::size_t index{}; index < shared_schema_object_count; ++index)
    {
        const auto object = objectId(index + 1U);
        assert(shared_schema_builder.addObject(object));
        assert(shared_schema_builder.addData(object, shared_schema, 1U, {}));
    }
    auto shared_schema_world = std::move(shared_schema_builder).build();
    assert(shared_schema_world);
    assert(shared_schema_world->schemas().size() == 1U);
    assert(
        shared_schema_world->retainedBytes() <
        shared_schema_object_count * 80U
    );

    WorldPartitionLayoutBuilder failure_layout_builder(*failure_world);
    const std::array failure_objects{objectId(8U)};
    detail::failNextWorldOperationForTest(
        detail::EWorldFailurePoint::PARTITION_MUTATION_ALLOCATION
    );
    const auto failed_partition = failure_layout_builder.addPartition(
        partitionId(8U),
        failure_objects
    );
    assert(!failed_partition);
    assert(
        failed_partition.error().code ==
        EWorldPartitionError::ALLOCATION_FAILURE
    );
    assert(failure_layout_builder.addPartition(
        partitionId(8U),
        failure_objects
    ));
    detail::failNextWorldOperationForTest(
        detail::EWorldFailurePoint::PARTITION_BUILD_ALLOCATION
    );
    const auto failed_layout_build = std::move(failure_layout_builder).build();
    assert(!failed_layout_build);
    assert(
        failed_layout_build.error().code ==
        EWorldPartitionError::ALLOCATION_FAILURE
    );
    detail::failNextWorldOperationForTest(
        detail::EWorldFailurePoint::PARTITION_BUILD_SIZE_OVERFLOW
    );
    const auto overflow_layout_build = std::move(failure_layout_builder).build();
    assert(!overflow_layout_build);
    assert(
        overflow_layout_build.error().code ==
        EWorldPartitionError::SIZE_OVERFLOW
    );
    const auto recovered_layout = std::move(failure_layout_builder).build();
    assert(recovered_layout && recovered_layout->partitionCount() == 1U);

    WorldDescriptionBuilder empty_builder;
    auto empty = std::move(empty_builder).build();
    assert(empty && empty->empty());
    WorldPartitionLayoutBuilder empty_layout_builder(*empty);
    auto empty_layout = std::move(empty_layout_builder).build();
    assert(empty_layout && empty_layout->empty());

    WorldDescription world = makeWorld();
    assert(world.objectCount() == 4U);
    assert(world.objectAt(0U).id() == objectId(1U));
    assert(world.findObject(objectId(3U)));
    assert(!world.findObject(objectId(9U)));
    assert(world.objectAt(0U).dataAt(0U).payload().size() == 2U);

    WorldPartitionLayoutBuilder missing_builder(world);
    const std::array first{objectId(1U)};
    assert(missing_builder.addPartition(partitionId(2U), first));
    auto missing = std::move(missing_builder).build();
    assert(!missing);
    assert(missing.error().code == EWorldPartitionError::MISSING_OBJECT_ASSIGNMENT);

    WorldPartitionLayoutBuilder duplicate_builder(world);
    const std::array duplicate{objectId(1U), objectId(1U)};
    auto duplicate_result = duplicate_builder.addPartition(partitionId(1U), duplicate);
    assert(!duplicate_result);
    assert(
        duplicate_result.error().code ==
        EWorldPartitionError::DUPLICATE_OBJECT_ASSIGNMENT
    );

    SinglePartitioner single;
    auto single_workspace = single.createWorkspace(world);
    assert(single_workspace);
    assert(
        (*single_workspace)->state() ==
        EWorldPartitionWorkspaceState::SYNCHRONIZED
    );
    auto single_product = (*single_workspace)->freeze(world);
    assert(single_product);
    assert(single_product->layout().partitionCount() == 1U);

    QuadtreeWorkspace incremental;
    assert(incremental.rebuild(world));
    auto quadtree_product = incremental.freeze(world);
    assert(quadtree_product);
    assert(quadtree_product->layout().partitionCount() > 1U);
    assert(quadtree_product->findIndex(
        worldPartitionIndexTypeId("test.quadtree.ordinal")
    ));

    // Two interpreters derive different products from the same immutable World.
    assert(
        quadtree_product->layout().partitionCount() !=
        single_product->layout().partitionCount()
    );

    const auto object = world.objectAt(0U);
    const auto data = object.dataAt(0U);
    const WorldDataSnapshotView snapshot_data{
        &data.schema(),
        data.version(),
        data.payload()};
    const WorldObjectSnapshotView snapshot{
        object.id(),
        std::span(&snapshot_data, 1U)};
    assert(incremental.objectChanged(snapshot));
    assert(incremental.objectRemoved(object.id()));
    assert(incremental.objectAdded(snapshot));
    auto after_incremental = incremental.freeze(world);
    assert(after_incremental);
    assert(
        after_incremental->layout().partitionCount() ==
        quadtree_product->layout().partitionCount()
    );

    incremental.failNextIncrementalForTest();
    const auto poisoned = incremental.objectChanged(snapshot);
    assert(!poisoned);
    assert(
        incremental.state() == EWorldPartitionWorkspaceState::STALE
    );
    const auto stale_freeze = incremental.freeze(world);
    assert(!stale_freeze);
    assert(
        stale_freeze.error().code == EWorldPartitionError::WORKSPACE_STALE
    );
    const auto stale_incremental = incremental.objectAdded(snapshot);
    assert(!stale_incremental);
    assert(
        stale_incremental.error().code == EWorldPartitionError::WORKSPACE_STALE
    );
    assert(incremental.rebuild(world));
    assert(
        incremental.state() ==
        EWorldPartitionWorkspaceState::SYNCHRONIZED
    );
    assert(incremental.freeze(world));

    return 0;
}
