#pragma once

#include <lux/engine/ecs/SystemAccessSpec.hpp>
#include <lux/engine/ecs/SystemConcept.hpp>
#include <lux/engine/task/Task.hpp>

#include <cstdint>
#include <string_view>

namespace lux::ecs
{
    namespace detail
    {
        [[nodiscard]] consteval std::uint64_t taskDomainHash(
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

        inline constexpr std::uint64_t kComponentTaskDomain =
            taskDomainHash("lux.ecs.component");
        inline constexpr std::uint64_t kExternalTaskDomain =
            taskDomainHash("lux.ecs.external");
    }

    [[nodiscard]] inline task::TaskResourceKey componentTaskResource(
        std::uint64_t storage
    ) noexcept
    {
        return {detail::kComponentTaskDomain, storage};
    }

    [[nodiscard]] inline task::TaskResourceKey externalTaskResource(
        std::uint64_t type_hash
    ) noexcept
    {
        return {detail::kExternalTaskDomain, type_hash};
    }

    /** Converts ECS access metadata into generic L0 Task resource metadata. */
    [[nodiscard]] inline task::TaskResources systemTaskResources(
        SystemAccessSpec access
    )
    {
        task::TaskResources result;
        result.values.reserve(access.components.size() + access.external.size());

        for (const auto& component : access.components)
        {
            const auto key = componentTaskResource(component.storage);
            result.values.push_back(
                component.mode == ESystemAccessMode::WRITE
                    ? task::write(key)
                    : task::read(key)
            );
        }
        for (const auto& external : access.external)
        {
            const auto key = externalTaskResource(external.type.hash());
            result.values.push_back(
                external.mode == ESystemAccessMode::WRITE
                    ? task::write(key)
                    : task::read(key)
            );
        }
        return result;
    }

    template <System Type>
    [[nodiscard]] task::TaskResources systemTaskResources()
    {
        return systemTaskResources(Type::Access.spec());
    }
}
