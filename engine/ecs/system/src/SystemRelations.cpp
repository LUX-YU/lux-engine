#include <lux/engine/ecs/SystemRelations.hpp>

#include <lux/engine/ecs/system/detail/SystemRelationsAccess.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        lux::cxx::ScopeIdSource<SystemRelationsScopeTag> g_relation_ids;
    }

    struct SystemRelations::Impl final
    {
        SystemRelationsId id{g_relation_ids.acquire()};
        std::vector<detail::SystemRelationEdge> relations;
        std::uint64_t revision{1U};
    };

    SystemRelations::SystemRelations()
        : impl_(std::make_unique<Impl>())
    {
    }

    SystemRelations::~SystemRelations() = default;
    SystemRelations::SystemRelations(SystemRelations&& other) noexcept
        : impl_(std::move(other.impl_))
    {
    }

    SystemRelations& SystemRelations::operator=(
        SystemRelations&& other
    ) noexcept
    {
        if (this != std::addressof(other))
            impl_ = std::move(other.impl_);
        return *this;
    }

    lux::cxx::expected<void, SystemFailure> SystemRelations::before(
        SystemId before,
        SystemId after
    ) noexcept
    {
        if (!before.isValid() || !after.isValid() || before == after)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::INVALID_SYSTEM,
                .system = before,
                .related = after
            });
        }

        try
        {
            if (!impl_)
                impl_ = std::make_unique<Impl>();
            const detail::SystemRelationEdge relation{before, after};
            if (std::find(
                impl_->relations.begin(),
                impl_->relations.end(),
                relation
            ) != impl_->relations.end())
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::DUPLICATE_RELATION,
                    .system = before,
                    .related = after
                });
            }

            impl_->relations.push_back(relation);
            ++impl_->revision;
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE,
                .system = before,
                .related = after
            });
        }
    }

    lux::cxx::expected<void, SystemFailure> SystemRelations::after(
        SystemId after,
        SystemId before
    ) noexcept
    {
        return this->before(before, after);
    }

    SystemRelationsId SystemRelations::id() const noexcept
    {
        return impl_ ? impl_->id : SystemRelationsId{};
    }

    std::size_t SystemRelations::size() const noexcept
    {
        return impl_ ? impl_->relations.size() : 0U;
    }

    std::uint64_t SystemRelations::revision() const noexcept
    {
        return impl_ ? impl_->revision : 0U;
    }

    namespace detail
    {
        SystemRelationsId SystemRelationsAccess::scope(
            const SystemRelations& relations
        ) noexcept
        {
            return relations.impl_ ? relations.impl_->id : SystemRelationsId{};
        }

        std::span<const SystemRelationEdge> SystemRelationsAccess::edges(
            const SystemRelations& relations
        ) noexcept
        {
            return relations.impl_ == nullptr
                ? std::span<const SystemRelationEdge>{}
                : std::span<const SystemRelationEdge>{
                    relations.impl_->relations
                };
        }
    }
}
