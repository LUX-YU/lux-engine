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
    inline constexpr std::uint64_t EcsStructureTaskDomain =
        detail::ecsTaskDomainHash("lux.ecs.structure");
    inline constexpr std::uint64_t EcsChangesTaskDomain =
        detail::ecsTaskDomainHash("lux.ecs.changes");
    inline constexpr std::uint64_t EcsCommandsTaskDomain =
        detail::ecsTaskDomainHash("lux.ecs.commands");

    static_assert(ComponentTaskDomain != ExternalTaskDomain);
    static_assert(ComponentTaskDomain != EcsStructureTaskDomain);
    static_assert(ComponentTaskDomain != EcsChangesTaskDomain);
    static_assert(ComponentTaskDomain != EcsCommandsTaskDomain);
    static_assert(ExternalTaskDomain != EcsStructureTaskDomain);
    static_assert(ExternalTaskDomain != EcsChangesTaskDomain);
    static_assert(ExternalTaskDomain != EcsCommandsTaskDomain);
    static_assert(EcsStructureTaskDomain != EcsChangesTaskDomain);
    static_assert(EcsStructureTaskDomain != EcsCommandsTaskDomain);
    static_assert(EcsChangesTaskDomain != EcsCommandsTaskDomain);

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
    ecsStructureTaskResource() noexcept
    {
        return {EcsStructureTaskDomain, 1U};
    }

    [[nodiscard]] constexpr task::TaskResourceKey
    ecsChangesTaskResource() noexcept
    {
        return {EcsChangesTaskDomain, 1U};
    }

    [[nodiscard]] constexpr task::TaskResourceKey
    ecsCommandsTaskResource() noexcept
    {
        return {EcsCommandsTaskDomain, 1U};
    }
}
