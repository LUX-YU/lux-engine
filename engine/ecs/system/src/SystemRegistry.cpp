#include <lux/engine/ecs/SystemRegistry.hpp>

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/system/detail/SystemRegistryAccess.hpp>

#include <lux/cxx/container/BasicSparseSet.hpp>

#include <thread>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        lux::cxx::ScopeIdSource<SystemRegistryScopeTag> g_registry_ids;
    }

    struct SystemRegistry::Impl final
    {
        SystemRegistryId id{g_registry_ids.acquire()};
        lux::cxx::SlotKeyAutoSparseSet<SystemSlot, detail::SystemRecord>
            systems;
        std::thread::id owner_thread{std::this_thread::get_id()};
        std::uint64_t revision{1U};
        std::uint64_t next_registration_order{};
        bool executing{};
    };

    SystemRegistry::SystemRegistry()
        : impl_(std::make_unique<Impl>())
    {
    }

    SystemRegistry::~SystemRegistry()
    {
        if (impl_)
        {
            detail::require(!impl_->executing);
            detail::require(impl_->owner_thread == std::this_thread::get_id());
        }
    }

    SystemRegistry::SystemRegistry(SystemRegistry&& other) noexcept
        : impl_(std::move(other.impl_))
    {
    }

    SystemRegistry& SystemRegistry::operator=(SystemRegistry&& other) noexcept
    {
        if (this == std::addressof(other))
            return *this;
        if (impl_)
        {
            detail::require(!impl_->executing);
            detail::require(impl_->owner_thread == std::this_thread::get_id());
        }
        impl_ = std::move(other.impl_);
        return *this;
    }

    lux::cxx::expected<SystemId, SystemFailure> SystemRegistry::add(
        detail::SystemRecord record
    ) noexcept
    {
        try
        {
            if (!impl_)
                impl_ = std::make_unique<Impl>();
            detail::require(
                impl_->owner_thread == std::this_thread::get_id() &&
                !impl_->executing
            );
            if (!record.type.isValid() || record.object == nullptr ||
                record.destroy == nullptr || record.update == nullptr ||
                record.affinity_valid == nullptr)
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::INVALID_SYSTEM
                });
            }

            for (const auto& existing : impl_->systems.values())
            {
                if (existing.type.hash() == record.type.hash() &&
                    existing.type.name() != record.type.name())
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::TYPE_COLLISION
                    });
                }
            }

            record.registration_order = impl_->next_registration_order++;
            const SystemSlot slot = impl_->systems.emplace(std::move(record));
            ++impl_->revision;
            return SystemId{impl_->id, slot};
        }
        catch (...)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE
            });
        }
    }

    SystemRegistryId SystemRegistry::id() const noexcept
    {
        return impl_ ? impl_->id : SystemRegistryId{};
    }

    bool SystemRegistry::contains(SystemId id) const noexcept
    {
        return impl_ && id.owner == impl_->id &&
            impl_->systems.contains(id.slot);
    }

    std::size_t SystemRegistry::size() const noexcept
    {
        return impl_ ? impl_->systems.size() : 0U;
    }

    std::uint64_t SystemRegistry::revision() const noexcept
    {
        return impl_ ? impl_->revision : 0U;
    }

    bool SystemRegistry::erase(SystemId id) noexcept
    {
        if (!impl_ || id.owner != impl_->id)
            return false;
        detail::require(
            impl_->owner_thread == std::this_thread::get_id() &&
            !impl_->executing
        );
        if (!impl_->systems.erase(id.slot))
            return false;
        ++impl_->revision;
        return true;
    }

    namespace detail
    {
        SystemRegistryId SystemRegistryAccess::scope(
            const SystemRegistry& registry
        ) noexcept
        {
            return registry.impl_ ? registry.impl_->id : SystemRegistryId{};
        }

        const SystemRecord* SystemRegistryAccess::record(
            const SystemRegistry& registry,
            SystemId id
        ) noexcept
        {
            if (!registry.impl_ || id.owner != registry.impl_->id)
                return nullptr;
            return registry.impl_->systems.tryGet(id.slot);
        }

        SystemRecord* SystemRegistryAccess::record(
            SystemRegistry& registry,
            SystemId id
        ) noexcept
        {
            if (!registry.impl_ || id.owner != registry.impl_->id)
                return nullptr;
            return registry.impl_->systems.tryGet(id.slot);
        }

        SystemRecord* SystemRegistryAccess::record(
            SystemRegistry& registry,
            SystemSlot slot
        ) noexcept
        {
            return registry.impl_ == nullptr
                ? nullptr
                : registry.impl_->systems.tryGet(slot);
        }

        std::span<const SystemSlot> SystemRegistryAccess::slots(
            const SystemRegistry& registry
        ) noexcept
        {
            return registry.impl_ == nullptr
                ? std::span<const SystemSlot>{}
                : std::span<const SystemSlot>{registry.impl_->systems.keys()};
        }

        std::span<const SystemRecord> SystemRegistryAccess::records(
            const SystemRegistry& registry
        ) noexcept
        {
            return registry.impl_ == nullptr
                ? std::span<const SystemRecord>{}
                : std::span<const SystemRecord>{registry.impl_->systems.values()};
        }

        bool SystemRegistryAccess::acquireExecution(
            SystemRegistry& registry
        ) noexcept
        {
            if (!registry.impl_ || registry.impl_->executing ||
                registry.impl_->owner_thread != std::this_thread::get_id())
            {
                return false;
            }
            registry.impl_->executing = true;
            return true;
        }

        void SystemRegistryAccess::releaseExecution(
            SystemRegistry& registry
        ) noexcept
        {
            require(registry.impl_ && registry.impl_->executing);
            require(
                registry.impl_->owner_thread == std::this_thread::get_id()
            );
            registry.impl_->executing = false;
        }
    }
}
