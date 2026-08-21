#include <lux/engine/ecs/Registry.hpp>

namespace lux::ecs
{
	Registry::Registry()
		: Registry(RegistryMemoryResource::create())
	{}

    Registry::Registry(IRegistryMemoryUpstream& upstream)
        : Registry(RegistryMemoryResource::create(&upstream))
    {}

    Registry::Registry(
        std::shared_ptr<RegistryMemoryResource> resource) noexcept
        : detail::RegistryMemoryOwner(resource),
          RegistryBase(RegistryAllocator<Entity>{std::move(resource)})
    {}

    RegistryPublicationReservationResult
    Registry::reservePublication(std::size_t bytes) noexcept
    {
        return registryMemoryResource()->reservePublication(bytes);
    }

    RegistryPublicationAdmissionScope
    Registry::closePublicationAdmission() noexcept
    {
        return registryMemoryResource()->closePublicationAdmission();
    }

    RegistryMemorySnapshot Registry::memorySnapshot() const noexcept
    {
        return registryMemoryResource()->snapshot();
    }
}
