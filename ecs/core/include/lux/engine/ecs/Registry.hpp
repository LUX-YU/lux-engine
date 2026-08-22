#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/RegistryMemoryResource.hpp>
#include <lux/engine/ecs/visibility.h>

#include <entt/entity/handle.hpp>
#include <entt/entity/registry.hpp>

#include <memory>

namespace lux::ecs
{
    using RegistryBase = entt::basic_registry<
        Entity,
        RegistryAllocator<Entity>>;
    using EntityHandle = entt::basic_handle<RegistryBase>;
    using ConstEntityHandle = entt::basic_handle<const RegistryBase>;

    namespace detail
    {
        class RegistryMemoryOwner
        {
        protected:
            explicit RegistryMemoryOwner(
                std::shared_ptr<RegistryMemoryResource> resource) noexcept
                : resource_(std::move(resource))
            {}

            [[nodiscard]] const std::shared_ptr<RegistryMemoryResource>&
            registryMemoryResource() const noexcept
            {
                return resource_;
            }

        private:
            std::shared_ptr<RegistryMemoryResource> resource_;
        };
    }

    class LUX_ECS_PUBLIC Registry final
        : private detail::RegistryMemoryOwner,
          public RegistryBase
    {
    public:
        Registry();
        explicit Registry(IRegistryMemoryUpstream& upstream);

        Registry(const Registry&) = delete;
        Registry& operator=(const Registry&) = delete;
        Registry(Registry&&) = delete;
        Registry& operator=(Registry&&) = delete;

        [[nodiscard]] RegistryPublicationReservationResult
        reservePublication(std::size_t bytes) noexcept;

        [[nodiscard]] RegistryPublicationAdmissionScope
        closePublicationAdmission() noexcept;

        [[nodiscard]] RegistryMemorySnapshot memorySnapshot() const noexcept;

    private:
        explicit Registry(
            std::shared_ptr<RegistryMemoryResource> resource) noexcept;
    };
}
