#include <lux/engine/ecs/SystemRegistry.hpp>

#include <lux/engine/ecs/system/detail/SystemRegistryAccess.hpp>

#include <lux/cxx/container/BasicSparseSet.hpp>

#include <utility>

namespace lux::ecs
{
    struct SystemRegistry::Impl final
    {
        lux::cxx::SlotKeyAutoSparseSet<
            SystemId,
            std::shared_ptr<detail::SystemRecord>
        > systems;
        std::uint64_t revision{1};
        std::uint64_t next_registration_order{};
    };

    SystemRegistry::SystemRegistry()
        : impl_(std::make_unique<Impl>())
    {
    }

    SystemRegistry::~SystemRegistry() = default;
    SystemRegistry::SystemRegistry(SystemRegistry&&) noexcept = default;
    SystemRegistry& SystemRegistry::operator=(SystemRegistry&&) noexcept = default;

    lux::cxx::expected<SystemId, SystemFailure> SystemRegistry::add(
        detail::SystemRecord record
    ) noexcept
    {
        if (!impl_ || !record.type.isValid() || !record.object ||
            record.update == nullptr || record.affinity_valid == nullptr)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::INVALID_SYSTEM
            });
        }

        for (const auto& existing : impl_->systems.values())
        {
            if (existing->type.hash() == record.type.hash() &&
                existing->type.name() != record.type.name())
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::TYPE_COLLISION
                });
            }
        }

        try
        {
            record.registration_order = impl_->next_registration_order++;
            auto owned = std::make_shared<detail::SystemRecord>(
                std::move(record)
            );
            const SystemId id = impl_->systems.emplace(std::move(owned));
            ++impl_->revision;
            return id;
        }
        catch (...)
        {
            return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                .code = ESystemError::ALLOCATION_FAILURE
            });
        }
    }

    bool SystemRegistry::contains(SystemId id) const noexcept
    {
        return impl_ && impl_->systems.contains(id);
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
        if (!impl_ || !impl_->systems.erase(id))
            return false;
        ++impl_->revision;
        return true;
    }

    bool SystemRegistry::requestStop(SystemId id) noexcept
    {
        if (!impl_)
            return false;
        const auto record = impl_->systems.tryGet(id);
        if (record == nullptr)
            return false;
        (*record)->request_stop((*record)->object.get());
        return true;
    }

    bool SystemRegistry::stopped(SystemId id) const noexcept
    {
        if (!impl_)
            return false;
        const auto record = impl_->systems.tryGet(id);
        return record != nullptr &&
            (*record)->stopped((*record)->object.get());
    }

    namespace detail
    {
        std::shared_ptr<SystemRecord> SystemRegistryAccess::record(
            const SystemRegistry& registry,
            SystemId id
        ) noexcept
        {
            if (!registry.impl_)
                return {};
            const auto found = registry.impl_->systems.tryGet(id);
            return found == nullptr ? nullptr : *found;
        }

        std::span<const SystemId> SystemRegistryAccess::ids(
            const SystemRegistry& registry
        ) noexcept
        {
            return registry.impl_ == nullptr
                ? std::span<const SystemId>{}
                : std::span<const SystemId>{registry.impl_->systems.keys()};
        }

        std::span<const std::shared_ptr<SystemRecord>>
        SystemRegistryAccess::records(const SystemRegistry& registry) noexcept
        {
            return registry.impl_ == nullptr
                ? std::span<const std::shared_ptr<SystemRecord>>{}
                : std::span<const std::shared_ptr<SystemRecord>>{
                    registry.impl_->systems.values()
                };
        }
    }
}
