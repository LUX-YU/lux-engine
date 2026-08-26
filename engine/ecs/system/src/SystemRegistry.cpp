#include <lux/engine/ecs/SystemRegistry.hpp>

#include <lux/engine/ecs/World.hpp>

#include <lux/cxx/container/BasicSparseSet.hpp>

#include <thread>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        lux::cxx::ScopeIdSource<SystemRegistryScopeTag> g_registry_ids;

        struct SystemRecord final
        {
            lux::cxx::TypeToken type;
            std::shared_ptr<void> object;
        };
    }

    struct SystemRegistry::Impl final
    {
        SystemRegistryId id{g_registry_ids.acquire()};
        lux::cxx::SlotKeyAutoSparseSet<SystemSlot, SystemRecord> systems;
        std::thread::id owner_thread{std::this_thread::get_id()};
        std::uint64_t revision{1U};
    };

    SystemRegistry::SystemRegistry() : impl_(std::make_unique<Impl>()) {}

    SystemRegistry::~SystemRegistry()
    {
        if (impl_)
            detail::require(impl_->owner_thread == std::this_thread::get_id());
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
            detail::require(impl_->owner_thread == std::this_thread::get_id());
        impl_ = std::move(other.impl_);
        return *this;
    }

    lux::cxx::expected<SystemId, SystemFailure> SystemRegistry::add(
        lux::cxx::TypeToken type,
        std::shared_ptr<void> object
    ) noexcept
    {
        try
        {
            if (!impl_)
                impl_ = std::make_unique<Impl>();
            detail::require(impl_->owner_thread == std::this_thread::get_id());
            if (!type.isValid() || !object)
                return lux::cxx::unexpected(SystemFailure{});

            for (const auto& existing : impl_->systems.values())
            {
                if (existing.type.hash() == type.hash() &&
                    existing.type.name() != type.name())
                {
                    return lux::cxx::unexpected(SystemFailure{
                        .code = ESystemError::TYPE_COLLISION
                    });
                }
            }

            const auto slot = impl_->systems.emplace(SystemRecord{
                type,
                std::move(object)
            });
            ++impl_->revision;
            return SystemId{impl_->id, slot};
        }
        catch (...)
        {
            return lux::cxx::unexpected(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE
            });
        }
    }

    std::shared_ptr<void> SystemRegistry::retainErased(
        SystemId id,
        lux::cxx::TypeToken type
    ) noexcept
    {
        if (!impl_ || id.owner != impl_->id ||
            impl_->owner_thread != std::this_thread::get_id())
        {
            return {};
        }
        const auto* record = impl_->systems.tryGet(id.slot);
        if (record == nullptr || record->type != type)
            return {};
        return record->object;
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
        detail::require(impl_->owner_thread == std::this_thread::get_id());
        if (!impl_->systems.erase(id.slot))
            return false;
        ++impl_->revision;
        return true;
    }
}
