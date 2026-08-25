#pragma once

#include <lux/engine/ecs/World.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>

namespace lux::ecs
{
    namespace detail
    {
        struct SystemExecutionAccess;
    }

    enum class ESystemStartError : std::uint8_t
    {
        REJECTED,
        ALLOCATION_FAILURE
    };

    struct SystemStartError final
    {
        ESystemStartError code{ESystemStartError::REJECTED};
    };

    class SystemStart final
    {
    public:
        [[nodiscard]] bool boundTo(const World& world) const noexcept
        {
            return world_ == std::addressof(world);
        }

        [[nodiscard]] bool valid(Entity entity) const noexcept
        {
            return world_->valid(entity);
        }

        template <class Component>
        [[nodiscard]] const Component* find(Entity entity) const noexcept
        {
            return world_->template find<Component>(entity);
        }

        template <class Component>
        [[nodiscard]] const Component& get(Entity entity) const noexcept
        {
            return world_->template get<Component>(entity);
        }

        template <class... Access>
        [[nodiscard]] auto query() const
        {
            static_assert((!detail::AccessTraits<Access>::kWrite && ...));
            return world_->template query<Access...>();
        }

    private:
        explicit SystemStart(const World& world) noexcept : world_(&world) {}

        const World* world_{};

        friend class EcsExecutionContext;
        friend struct detail::SystemExecutionAccess;
    };
}
