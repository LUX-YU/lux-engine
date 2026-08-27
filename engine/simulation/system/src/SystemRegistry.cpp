#include <lux/engine/simulation/SystemRegistry.hpp>

#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/cxx/container/ScopeId.hpp>

#include <cstdlib>
#include <thread>
#include <utility>

namespace lux::simulation
{
    namespace
    {
        lux::cxx::ScopeIdSource<SystemRegistryScopeTag> g_registry_ids;

        [[noreturn]] void contractFailure() noexcept
        {
            std::abort();
        }
    }

    struct SystemRegistry::Impl final
    {
        SystemRegistryId id{g_registry_ids.acquire()};
        lux::cxx::SlotKeyAutoSparseSet<SystemSlot, std::shared_ptr<detail::SystemRecord>> systems;
        std::thread::id owner_thread{std::this_thread::get_id()};
        std::uint64_t revision{1U};
    };

    SystemRegistry::SystemRegistry() : impl_(std::make_unique<Impl>())
    {
    }

    SystemRegistry::~SystemRegistry()
    {
        if (impl_ && impl_->owner_thread != std::this_thread::get_id())
            contractFailure();
    }

    SystemRegistry::SystemRegistry(SystemRegistry&& other) noexcept : impl_(std::move(other.impl_))
    {
    }

    SystemRegistry& SystemRegistry::operator=(SystemRegistry&& other) noexcept
    {
        if (this == std::addressof(other))
            return *this;
        if (impl_ && impl_->owner_thread != std::this_thread::get_id())
            contractFailure();
        impl_ = std::move(other.impl_);
        return *this;
    }

    lux::cxx::expected<SystemId, SystemFailure>
    SystemRegistry::add(std::shared_ptr<detail::SystemRecord> record) noexcept
    {
        if (!impl_ || impl_->owner_thread != std::this_thread::get_id())
            contractFailure();
        const bool is_missing_record = record == nullptr;
        const bool is_invalid_type = !is_missing_record && !record->type.isValid();
        const bool is_missing_object = !is_missing_record && record->object == nullptr;
        const bool is_missing_destroy = !is_missing_record && record->destroy == nullptr;
        const bool is_invalid_record = is_missing_record || is_invalid_type || is_missing_object || is_missing_destroy;
        if (is_invalid_record)
        {
            return lux::cxx::unexpected(SystemFailure{.code = ESystemError::INVALID_SYSTEM});
        }

        try
        {
            for (const auto& existing : impl_->systems.values())
            {
                if (!existing)
                    continue;
                if (existing->type.hash() == record->type.hash() && existing->type.name() != record->type.name())
                {
                    return lux::cxx::unexpected(SystemFailure{.code = ESystemError::TYPE_COLLISION});
                }
            }

            const SystemSlot slot = impl_->systems.emplace(std::move(record));
            ++impl_->revision;
            return SystemId{impl_->id, slot};
        }
        catch (...)
        {
            return lux::cxx::unexpected(SystemFailure{.code = ESystemError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<std::shared_ptr<detail::SystemRecord>, SystemFailure>
    SystemRegistry::retainRecord(SystemId id) const noexcept
    {
        if (!impl_ || id.owner != impl_->id)
        {
            return lux::cxx::unexpected(SystemFailure{.code = ESystemError::INVALID_SYSTEM, .system = id});
        }
        const auto* record = impl_->systems.tryGet(id.slot);
        if (record == nullptr || !*record)
        {
            return lux::cxx::unexpected(SystemFailure{.code = ESystemError::INVALID_SYSTEM, .system = id});
        }
        return *record;
    }

    SystemRegistryId SystemRegistry::id() const noexcept
    {
        return impl_ ? impl_->id : SystemRegistryId{};
    }

    bool SystemRegistry::contains(SystemId id) const noexcept
    {
        return impl_ && id.owner == impl_->id && impl_->systems.contains(id.slot);
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
        if (!impl_ || impl_->owner_thread != std::this_thread::get_id())
            contractFailure();
        if (id.owner != impl_->id || !impl_->systems.erase(id.slot))
            return false;
        ++impl_->revision;
        return true;
    }
}
