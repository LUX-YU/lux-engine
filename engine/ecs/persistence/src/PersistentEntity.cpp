#include <lux/engine/ecs/PersistentEntity.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/serialization/external_support/Uuid.hpp>
#include <lux/engine/ecs/PersistentEntity.ecs_schema.hpp>
#include <lux/engine/ecs/PersistentEntity.ecs_load.hpp>
#include <algorithm>
#include <array>

namespace lux::ecs
{
    namespace
    {
        [[nodiscard]] auto uuidBytes(const PersistentEntityId& id) noexcept
        {
            return id.value.as_bytes();
        }

        [[nodiscard]] bool lessId(
            const PersistentEntityId& left,
            const PersistentEntityId& right
        ) noexcept
        {
            const auto lhs = uuidBytes(left);
            const auto rhs = uuidBytes(right);
            return std::lexicographical_compare(
                lhs.begin(), lhs.end(), rhs.begin(), rhs.end()
            );
        }

    } // namespace

    lux::cxx::expected<PersistentEntityIndex, EPersistentEntityIndexError>
    PersistentEntityIndex::build(const World& world) noexcept
    {
        if (!detail::WorldColdAccess::ownerIdle(world))
        {
            return lux::cxx::unexpected(
                EPersistentEntityIndexError::WORLD_BUSY
            );
        }
        try
        {
            PersistentEntityIndex result;
            for (auto [entity, id] : world.query<Read<PersistentId>>())
            {
                if (id.value.value.is_nil())
                    return lux::cxx::unexpected(EPersistentEntityIndexError::INVALID_ID);
                result.entries_.push_back({id.value, entity});
            }
            std::sort(
                result.entries_.begin(), result.entries_.end(),
                [](const auto& left, const auto& right)
                {
                    return lessId(left.first, right.first);
                }
            );
            for (std::size_t index = 1; index < result.entries_.size(); ++index)
            {
                if (result.entries_[index - 1].first == result.entries_[index].first)
                    return lux::cxx::unexpected(EPersistentEntityIndexError::DUPLICATE_ID);
            }
            return result;
        }
        catch (...)
        {
            return lux::cxx::unexpected(EPersistentEntityIndexError::ALLOCATION_FAILURE);
        }
    }

    Entity PersistentEntityIndex::find(PersistentEntityId id) const noexcept
    {
        const auto iterator = std::lower_bound(
            entries_.begin(), entries_.end(), id,
            [](const auto& entry, const PersistentEntityId& value)
            {
                return lessId(entry.first, value);
            }
        );
        return iterator != entries_.end() && iterator->first == id
            ? iterator->second
            : NullEntity;
    }

    std::size_t PersistentEntityIndex::size() const noexcept
    {
        return entries_.size();
    }

    const ComponentSchema& persistentIdComponentSchema() noexcept
    {
        return generated::persistenceComponentSchemas().front();
    }

    ComponentLoadContribution persistentEntityComponentLoadContribution() noexcept
    {
        return generated::persistenceComponentLoadContribution();
    }
} // namespace lux::ecs
