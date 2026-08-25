#include <lux/engine/ecs/WorldSectionLoader.hpp>

#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/world_section/detail/ComponentLoadSerialization.hpp>
#include <lux/engine/ecs/world_section/detail/WorldSectionTransactionAccess.hpp>

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace lux::ecs
{
    WorldSectionInstance::WorldSectionInstance(
        WorldSectionInstance&& other
    ) noexcept
        : id_(std::move(other.id_)),
          entities_(std::move(other.entities_)),
          world_identity_(std::exchange(other.world_identity_, 0U)),
          state_(std::exchange(other.state_, EState::INACTIVE))
    {
        detail::require(state_ != EState::STAGED);
        other.id_ = {};
        other.entities_.clear();
    }

    WorldSectionInstance::~WorldSectionInstance() noexcept
    {
        detail::require(state_ == EState::INACTIVE);
    }

    namespace
    {
        [[nodiscard]] WorldSectionFailure failure(
            EWorldSectionError code,
            std::uint32_t column_index = 0U,
            ComponentSchemaId schema = {}
        )
        {
            WorldSectionFailure result;
            result.code = code;
            result.column_index = column_index;
            result.schema = std::move(schema);
            return result;
        }

        [[nodiscard]] EWorldSectionError mapSerializationError(
            lux::serialization::ESerializationError code
        ) noexcept
        {
            switch (code)
            {
            case lux::serialization::ESerializationError::LIMIT_EXCEEDED:
            case lux::serialization::ESerializationError::SIZE_OVERFLOW:
                return EWorldSectionError::LIMIT_EXCEEDED;
            case lux::serialization::ESerializationError::ALLOCATION_FAILURE:
                return EWorldSectionError::ALLOCATION_FAILURE;
            default:
                return EWorldSectionError::DECODE_FAILED;
            }
        }

        [[nodiscard]] WorldSectionFailure decodeFailure(
            const lux::serialization::SerializationFailure& source,
            std::uint32_t column_index,
            const ComponentSchemaId& schema
        )
        {
            WorldSectionFailure result = failure(
                mapSerializationError(source.code),
                column_index,
                schema
            );
            result.byte_offset = source.offset;
            return result;
        }
    } // namespace

    lux::cxx::expected<WorldSectionInstance, WorldSectionFailure>
    WorldSectionLoader::load(
        World& world,
        const ComponentLoadSet& loads,
        const WorldSectionImage& image,
        lux::serialization::SerializationLimits limits
    ) noexcept
    {
        if (!detail::WorldColdAccess::ownerIdle(world))
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::WORLD_BUSY)
            );
        }

        try
        {
            std::vector<const ComponentLoadBinding*> plan;
            plan.reserve(image.columns().size());
            std::size_t max_sparse_rows{};
            for (std::size_t index{}; index < image.columns().size(); ++index)
            {
                const auto& column = image.columns()[index];
                const ComponentLoadBinding* binding = loads.find(
                    column.schemaHash(),
                    column.schemaName()
                );
                if (binding == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldSectionError::MISSING_BINDING,
                        static_cast<std::uint32_t>(index),
                        componentSchemaId(column.schemaName())
                    ));
                }
                if (binding->schema().version != column.schemaVersion() ||
                    binding->valueEncoding() != column.valueEncoding() ||
                    (column.valueEncoding() ==
                         EWorldSectionValueEncoding::FIXED &&
                     binding->fixedStride() != column.fixedStride()))
                {
                    return lux::cxx::unexpected(failure(
                        EWorldSectionError::BINDING_MISMATCH,
                        static_cast<std::uint32_t>(index),
                        binding->schema().id
                    ));
                }
                plan.push_back(binding);
                if (column.ordinalEncoding() ==
                    EWorldSectionOrdinalEncoding::U32_LIST)
                {
                    max_sparse_rows = std::max(
                        max_sparse_rows,
                        static_cast<std::size_t>(column.rowCount())
                    );
                }
            }

            WorldSectionInstance instance;
            instance.id_ = image.id();
            instance.entities_.resize(
                image.entityCount(),
                NullEntity
            );
            std::vector<Entity> sparse_entities;
            sparse_entities.reserve(max_sparse_rows);

            auto edit = detail::WorldColdAccess::sectionEdit(world);
            try
            {
                detail::WorldSectionTransactionAccess::createEntities(
                    edit,
                    instance.entities_
                );
                for (std::size_t index{};
                     index < image.columns().size();
                     ++index)
                {
                    const auto& column = image.columns()[index];
                    if (column.rowCount() == 0U)
                        continue;

                    std::span<const Entity> row_entities;
                    if (column.ordinalEncoding() ==
                        EWorldSectionOrdinalEncoding::DENSE)
                    {
                        row_entities = instance.entities_;
                    }
                    else
                    {
                        sparse_entities.clear();
                        for (std::size_t row{}; row < column.rowCount(); ++row)
                        {
                            const std::uint32_t ordinal =
                                detail::readColumnU32(
                                    column.ordinalBytes(),
                                    row * sizeof(std::uint32_t)
                                );
                            sparse_entities.push_back(
                                instance.entities_[ordinal]
                            );
                        }
                        row_entities = sparse_entities;
                    }

                    auto loaded = plan[index]->load_(
                        edit,
                        row_entities,
                        instance.entities_,
                        column,
                        limits
                    );
                    if (!loaded)
                    {
                        detail::WorldSectionTransactionAccess::destroyValidEntities(
                            edit,
                            instance.entities_
                        );
                        edit = {};
                        return lux::cxx::unexpected(decodeFailure(
                            loaded.error(),
                            static_cast<std::uint32_t>(index),
                            plan[index]->schema().id
                        ));
                    }
                }
            }
            catch (const std::bad_alloc&)
            {
                detail::WorldSectionTransactionAccess::destroyValidEntities(
                    edit,
                    instance.entities_
                );
                edit = {};
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::ALLOCATION_FAILURE)
                );
            }
            catch (...)
            {
                detail::WorldSectionTransactionAccess::destroyValidEntities(
                    edit,
                    instance.entities_
                );
                edit = {};
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::DECODE_FAILED)
                );
            }

            edit = {};
            if (!instance.entities_.empty())
                detail::markWorldChangeHistoryLoss(world);
            instance.world_identity_ = detail::WorldColdAccess::identity(world);
            instance.state_ = WorldSectionInstance::EState::ACTIVE;
            detail::WorldColdAccess::acquireSection(world);
            return instance;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::DECODE_FAILED)
            );
        }
    }

    lux::cxx::expected<void, WorldSectionFailure>
    WorldSectionLoader::unload(
        World& world,
        WorldSectionInstance& instance
    ) noexcept
    {
        if (!detail::WorldColdAccess::ownerIdle(world))
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::WORLD_BUSY)
            );
        }
        if (instance.state_ != WorldSectionInstance::EState::ACTIVE)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::INVALID_INSTANCE_STATE)
            );
        }
        if (instance.world_identity_ != detail::WorldColdAccess::identity(world))
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::WRONG_WORLD)
            );
        }

        try
        {
            std::vector<Entity> live_entities;
            live_entities.reserve(instance.entities_.size());
            for (const Entity entity : instance.entities_)
            {
                if (world.valid(entity))
                    live_entities.push_back(entity);
            }

            auto edit = detail::WorldColdAccess::sectionEdit(world);
            detail::WorldSectionTransactionAccess::destroyEntities(
                edit,
                live_entities
            );
            edit = {};
            instance.entities_.clear();
            instance.id_ = {};
            instance.world_identity_ = 0U;
            instance.state_ = WorldSectionInstance::EState::INACTIVE;
            detail::WorldColdAccess::releaseSection(world);
            if (!live_entities.empty())
                detail::markWorldChangeHistoryLoss(world);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::DECODE_FAILED)
            );
        }
    }
} // namespace lux::ecs
