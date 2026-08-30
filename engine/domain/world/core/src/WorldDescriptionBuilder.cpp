#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/detail/WorldFailureInjection.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace lux::world
{
    using partition::PartitionIndexTypeId;
    using partition::PartitionOrdinal;

    struct WorldDescriptionBuilder::Impl final
    {
        WorldBundleId bundle;
        WorldBundleGeneration generation;
        std::string name;
        std::vector<WorldDataSchemaId> schemas;
        WorldPartitionerDescriptor partitioner;
        std::uint32_t partition_count{};
        std::vector<WorldStorageVolumeDescription> volumes;
        std::vector<WorldPartitionTablePageDescription> pages;
        std::vector<WorldPartitionIndexDescription> indexes;
        bool has_identity{};
        bool has_partitioner{};
    };

    namespace
    {
        [[nodiscard]] WorldDescriptionFailure failure(
            EWorldDescriptionError code,
            WorldDataSchemaId schema = {},
            PartitionOrdinal partition = {},
            PartitionIndexTypeId index_type = {},
            std::uint32_t volume = 0U
        ) noexcept
        {
            return WorldDescriptionFailure{
                code,
                std::move(schema),
                partition,
                std::move(index_type),
                volume
            };
        }

        [[nodiscard]] bool validMemberName(std::string_view name) noexcept
        {
            if (name.empty() || name.front() == '/' || name.front() == '\\' ||
                name.find('\\') != std::string_view::npos || name.find(':') != std::string_view::npos)
            {
                return false;
            }

            std::size_t first{};
            while (first <= name.size())
            {
                const std::size_t separator = name.find('/', first);
                const std::size_t last = separator == std::string_view::npos ? name.size() : separator;
                const std::string_view segment = name.substr(first, last - first);
                if (segment.empty() || segment == "." || segment == "..")
                    return false;
                if (separator == std::string_view::npos)
                    break;
                first = separator + 1U;
            }
            return true;
        }

        [[nodiscard]] bool validChunkReference(
            WorldChunkReference reference,
            std::span<const WorldStorageVolumeDescription> volumes
        ) noexcept
        {
            return reference.volume < volumes.size() &&
                   reference.chunk < volumes[reference.volume].chunk_count;
        }
    } // namespace

    WorldDescriptionBuilder::WorldDescriptionBuilder() : impl_(std::make_unique<Impl>())
    {
    }

    WorldDescriptionBuilder::~WorldDescriptionBuilder() = default;
    WorldDescriptionBuilder::WorldDescriptionBuilder(WorldDescriptionBuilder&&) noexcept = default;
    WorldDescriptionBuilder& WorldDescriptionBuilder::operator=(WorldDescriptionBuilder&&) noexcept = default;

    lux::cxx::expected<void, WorldDescriptionFailure> WorldDescriptionBuilder::setIdentity(
        WorldBundleId bundle,
        WorldBundleGeneration generation,
        std::string_view name
    ) noexcept
    {
        if (!bundle.valid())
            return lux::cxx::unexpected(failure(EWorldDescriptionError::INVALID_BUNDLE_ID));
        if (!generation.valid())
            return lux::cxx::unexpected(failure(EWorldDescriptionError::INVALID_GENERATION));
        if (name.empty())
            return lux::cxx::unexpected(failure(EWorldDescriptionError::INVALID_NAME));
        if (detail::consumeWorldFailureForTest(detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION))
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));

        try
        {
            std::string copied_name(name);
            impl_->bundle = bundle;
            impl_->generation = generation;
            impl_->name = std::move(copied_name);
            impl_->has_identity = true;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::addSchema(WorldDataSchemaId schema) noexcept
    {
        if (!schema.valid())
            return lux::cxx::unexpected(failure(EWorldDescriptionError::INVALID_SCHEMA_ID, std::move(schema)));

        for (const auto& existing : impl_->schemas)
        {
            if (existing.hash == schema.hash && existing.name != schema.name)
            {
                return lux::cxx::unexpected(
                    failure(EWorldDescriptionError::SCHEMA_HASH_COLLISION, std::move(schema))
                );
            }
            if (existing == schema)
                return lux::cxx::unexpected(failure(EWorldDescriptionError::DUPLICATE_SCHEMA, std::move(schema)));
        }
        if (detail::consumeWorldFailureForTest(detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION))
        {
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE, std::move(schema)));
        }

        try
        {
            impl_->schemas.push_back(std::move(schema));
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE, std::move(schema)));
        }
    }

    lux::cxx::expected<void, WorldDescriptionFailure> WorldDescriptionBuilder::setPartitioner(
        WorldPartitionerDescriptor partitioner,
        std::uint32_t partition_count
    ) noexcept
    {
        if (!partitioner.id.valid() || partitioner.version == 0U)
            return lux::cxx::unexpected(failure(EWorldDescriptionError::INVALID_PARTITIONER));

        impl_->partitioner = std::move(partitioner);
        impl_->partition_count = partition_count;
        impl_->has_partitioner = true;
        return {};
    }

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::addStorageVolume(WorldStorageVolumeDescription volume) noexcept
    {
        if (!validMemberName(volume.member_name) || volume.format_version == 0U)
        {
            return lux::cxx::unexpected(
                failure(
                    EWorldDescriptionError::INVALID_VOLUME,
                    {},
                    {},
                    {},
                    static_cast<std::uint32_t>(impl_->volumes.size())
                )
            );
        }
        for (const auto& existing : impl_->volumes)
        {
            if (existing.member_name == volume.member_name)
            {
                return lux::cxx::unexpected(
                    failure(
                        EWorldDescriptionError::DUPLICATE_VOLUME_MEMBER,
                        {},
                        {},
                        {},
                        static_cast<std::uint32_t>(impl_->volumes.size())
                    )
                );
            }
        }
        if (detail::consumeWorldFailureForTest(detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION))
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));

        try
        {
            impl_->volumes.push_back(std::move(volume));
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::addPartitionTablePage(WorldPartitionTablePageDescription page) noexcept
    {
        if (page.count == 0U)
        {
            return lux::cxx::unexpected(
                failure(EWorldDescriptionError::INVALID_PARTITION_PAGE, {}, page.first)
            );
        }
        if (detail::consumeWorldFailureForTest(detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION))
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));

        try
        {
            impl_->pages.push_back(page);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::addPartitionIndex(WorldPartitionIndexDescription index) noexcept
    {
        if (!index.type.valid() || index.version == 0U)
        {
            return lux::cxx::unexpected(
                failure(EWorldDescriptionError::INVALID_INDEX, {}, {}, std::move(index.type))
            );
        }
        for (const auto& existing : impl_->indexes)
        {
            if (existing.type.hash == index.type.hash && existing.type.name != index.type.name)
            {
                return lux::cxx::unexpected(
                    failure(EWorldDescriptionError::INVALID_INDEX, {}, {}, std::move(index.type))
                );
            }
            if (existing.type == index.type)
            {
                return lux::cxx::unexpected(
                    failure(EWorldDescriptionError::DUPLICATE_INDEX_TYPE, {}, {}, std::move(index.type))
                );
            }
        }
        if (detail::consumeWorldFailureForTest(detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION))
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));

        try
        {
            impl_->indexes.push_back(std::move(index));
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));
        }
    }

    void WorldDescriptionBuilder::clear() noexcept
    {
        *impl_ = Impl{};
    }

    lux::cxx::expected<WorldDescription, WorldDescriptionFailure> WorldDescriptionBuilder::build() && noexcept
    {
        if (!impl_->has_identity)
            return lux::cxx::unexpected(failure(EWorldDescriptionError::INVALID_BUNDLE_ID));
        if (!impl_->has_partitioner)
            return lux::cxx::unexpected(failure(EWorldDescriptionError::INVALID_PARTITIONER));
        if (detail::consumeWorldFailureForTest(detail::EWorldFailurePoint::DESCRIPTION_BUILD_ALLOCATION))
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));
        if (detail::consumeWorldFailureForTest(detail::EWorldFailurePoint::DESCRIPTION_BUILD_SIZE_OVERFLOW))
            return lux::cxx::unexpected(failure(EWorldDescriptionError::SIZE_OVERFLOW));

        try
        {
            std::sort(impl_->schemas.begin(), impl_->schemas.end(), WorldDataSchemaIdLess{});
            std::sort(
                impl_->pages.begin(),
                impl_->pages.end(),
                [](const WorldPartitionTablePageDescription& left,
                   const WorldPartitionTablePageDescription& right) noexcept {
                    return left.first.value < right.first.value;
                }
            );
            std::sort(
                impl_->indexes.begin(),
                impl_->indexes.end(),
                [](const WorldPartitionIndexDescription& left,
                   const WorldPartitionIndexDescription& right) noexcept {
                    return left.type.hash < right.type.hash ||
                           (left.type.hash == right.type.hash && left.type.name < right.type.name);
                }
            );

            std::uint64_t expected_partition{};
            for (const auto& page : impl_->pages)
            {
                if (page.first.value < expected_partition)
                {
                    return lux::cxx::unexpected(
                        failure(EWorldDescriptionError::PARTITION_PAGE_OVERLAP, {}, page.first)
                    );
                }
                if (page.first.value > expected_partition)
                {
                    return lux::cxx::unexpected(
                        failure(EWorldDescriptionError::PARTITION_PAGE_GAP, {}, page.first)
                    );
                }
                const std::uint64_t end = static_cast<std::uint64_t>(page.first.value) + page.count;
                if (end > impl_->partition_count)
                {
                    return lux::cxx::unexpected(
                        failure(EWorldDescriptionError::INVALID_PARTITION_PAGE, {}, page.first)
                    );
                }
                if (!validChunkReference(page.chunk, impl_->volumes))
                {
                    return lux::cxx::unexpected(
                        failure(
                            EWorldDescriptionError::INVALID_CHUNK_REFERENCE,
                            {},
                            page.first,
                            {},
                            page.chunk.volume
                        )
                    );
                }
                expected_partition = end;
            }
            if (expected_partition != impl_->partition_count)
            {
                return lux::cxx::unexpected(
                    failure(
                        EWorldDescriptionError::PARTITION_PAGE_GAP,
                        {},
                        PartitionOrdinal{static_cast<std::uint32_t>(expected_partition)}
                    )
                );
            }

            for (const auto& index : impl_->indexes)
            {
                if (!validChunkReference(index.root, impl_->volumes))
                {
                    return lux::cxx::unexpected(
                        failure(
                            EWorldDescriptionError::INVALID_CHUNK_REFERENCE,
                            {},
                            {},
                            index.type,
                            index.root.volume
                        )
                    );
                }
            }

            WorldDescription result;
            result.bundle_id_ = impl_->bundle;
            result.generation_ = impl_->generation;
            result.name_ = std::move(impl_->name);
            result.schemas_ = std::move(impl_->schemas);
            result.partitioner_ = std::move(impl_->partitioner);
            result.partition_count_ = impl_->partition_count;
            result.storage_volumes_ = std::move(impl_->volumes);
            result.partition_table_.pages_ = std::move(impl_->pages);
            result.partition_indexes_ = std::move(impl_->indexes);
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EWorldDescriptionError::ALLOCATION_FAILURE));
        }
    }
} // namespace lux::world
