#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldPartitioner.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::world;

    [[nodiscard]] WorldObjectId objectId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
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
        rebuild(const WorldDescription&) noexcept override
        {
            return {};
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        objectAdded(WorldObjectSnapshotView) noexcept override
        {
            return {};
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        objectChanged(WorldObjectSnapshotView) noexcept override
        {
            return {};
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        objectRemoved(WorldObjectId) noexcept override
        {
            return {};
        }

        lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>
        freeze(const WorldDescription& world) const noexcept override
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
        createWorkspace(const WorldDescription&) const noexcept override
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

        const WorldPartitionerDescriptor& descriptor() const noexcept override
        {
            return descriptor_;
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        rebuild(const WorldDescription& world) noexcept override
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
        objectAdded(WorldObjectSnapshotView object) noexcept override
        {
            return assign(object);
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        objectChanged(WorldObjectSnapshotView object) noexcept override
        {
            return assign(object);
        }

        lux::cxx::expected<void, WorldPartitionFailure>
        objectRemoved(WorldObjectId object) noexcept override
        {
            objects_.erase(object);
            return {};
        }

        lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>
        freeze(const WorldDescription& world) const noexcept override
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

    return 0;
}
