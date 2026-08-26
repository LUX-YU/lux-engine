#pragma once

#include <lux/engine/task/Task.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstdint>
#include <string_view>

namespace lux::ecs
{
    namespace detail
    {
        [[nodiscard]] consteval std::uint64_t ecsTaskDomainHash(
            std::string_view text
        ) noexcept
        {
            std::uint64_t value = 14695981039346656037ULL;
            for (const char ch : text)
            {
                value ^= static_cast<unsigned char>(ch);
                value *= 1099511628211ULL;
            }
            return value;
        }
    }

    inline constexpr std::uint64_t ComponentTaskDomain =
        detail::ecsTaskDomainHash("lux.ecs.component");
    inline constexpr std::uint64_t ExternalTaskDomain =
        detail::ecsTaskDomainHash("lux.ecs.external");
    inline constexpr std::uint64_t WorldStructureTaskDomain =
        detail::ecsTaskDomainHash("lux.ecs.world.structure");
    inline constexpr std::uint64_t WorldChangesTaskDomain =
        detail::ecsTaskDomainHash("lux.ecs.world.changes");
    inline constexpr std::uint64_t WorldCommandsTaskDomain =
        detail::ecsTaskDomainHash("lux.ecs.world.commands");

    static_assert(ComponentTaskDomain != ExternalTaskDomain);
    static_assert(ComponentTaskDomain != WorldStructureTaskDomain);
    static_assert(ComponentTaskDomain != WorldChangesTaskDomain);
    static_assert(ComponentTaskDomain != WorldCommandsTaskDomain);
    static_assert(ExternalTaskDomain != WorldStructureTaskDomain);
    static_assert(ExternalTaskDomain != WorldChangesTaskDomain);
    static_assert(ExternalTaskDomain != WorldCommandsTaskDomain);
    static_assert(WorldStructureTaskDomain != WorldChangesTaskDomain);
    static_assert(WorldStructureTaskDomain != WorldCommandsTaskDomain);
    static_assert(WorldChangesTaskDomain != WorldCommandsTaskDomain);

    [[nodiscard]] constexpr task::TaskResourceKey componentTaskResource(
        std::uint64_t storage
    ) noexcept
    {
        return {ComponentTaskDomain, storage};
    }

    [[nodiscard]] constexpr task::TaskResourceKey externalTaskResource(
        std::uint64_t type_hash
    ) noexcept
    {
        return {ExternalTaskDomain, type_hash};
    }

    template <class Resource>
    [[nodiscard]] constexpr task::TaskResourceKey
    externalTaskResource() noexcept
    {
        return externalTaskResource(
            lux::cxx::typeToken<Resource>().hash()
        );
    }

    [[nodiscard]] constexpr task::TaskResourceKey
    worldStructureTaskResource() noexcept
    {
        return {WorldStructureTaskDomain, 1U};
    }

    [[nodiscard]] constexpr task::TaskResourceKey
    worldChangesTaskResource() noexcept
    {
        return {WorldChangesTaskDomain, 1U};
    }

    [[nodiscard]] constexpr task::TaskResourceKey
    worldCommandsTaskResource() noexcept
    {
        return {WorldCommandsTaskDomain, 1U};
    }
}
