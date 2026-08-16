#include <lux/engine/meta/LuxObject.hpp>
#include <lux/engine/core/visibility.h>

namespace lux::meta
{
	EntityRegistry::EntityRegistry()
		: EntityRegistry(RegistryMemoryResource::create())
	{}

    EntityRegistry::EntityRegistry(IRegistryMemoryUpstream& upstream)
        : EntityRegistry(RegistryMemoryResource::create(&upstream))
    {}

    EntityRegistry::EntityRegistry(
        std::shared_ptr<RegistryMemoryResource> resource) noexcept
        : detail::EntityRegistryMemoryOwner(resource),
          EntityRegistryBase(RegistryAllocator<entity_id>{std::move(resource)})
    {}

    RegistryPublicationReservationResult
    EntityRegistry::reservePublication(std::size_t bytes) noexcept
    {
        return registryMemoryResource()->reservePublication(bytes);
    }

    RegistryPublicationAdmissionScope
    EntityRegistry::closePublicationAdmission() noexcept
    {
        return registryMemoryResource()->closePublicationAdmission();
    }

    RegistryMemorySnapshot EntityRegistry::memorySnapshot() const noexcept
    {
        return registryMemoryResource()->snapshot();
    }
}
