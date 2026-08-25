#include <lux/engine/ecs/SystemRelations.hpp>

#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/system/detail/SystemRelationsAccess.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::ecs
{
    struct SystemRelations::Impl final
    {
        const SystemRegistry* registry{};
        std::vector<detail::SystemRelationEdge> relations;
        std::uint64_t revision{1};
    };

    SystemRelations::SystemRelations(const SystemRegistry& registry)
        : impl_(std::make_unique<Impl>())
    {
        impl_->registry = std::addressof(registry);
    }

    SystemRelations::~SystemRelations() = default;
    SystemRelations::SystemRelations(SystemRelations&&) noexcept = default;
    SystemRelations& SystemRelations::operator=(SystemRelations&&) noexcept = default;

    lux::cxx::expected<void, SystemFailure> SystemRelations::before(
        SystemId before,
        SystemId after
    ) noexcept
    {
        if (!impl_ || before == after ||
            !impl_->registry->contains(before) ||
            !impl_->registry->contains(after))
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::INVALID_SYSTEM,
                .system = before,
                .related = after
            });
        }

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

        try
        {
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
        const SystemRegistry* SystemRelationsAccess::registry(
            const SystemRelations& relations
        ) noexcept
        {
            return relations.impl_ ? relations.impl_->registry : nullptr;
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
