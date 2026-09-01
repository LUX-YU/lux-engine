#include <lux/engine/simulation/SimulationSystemRegistry.hpp>

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace lux::simulation
{
    struct SimulationSystemRegistry::Impl final
    {
        std::vector<SimulationSystemRegistration> registrations;
    };

    namespace
    {
        [[nodiscard]] bool lessRegistration(
            const SimulationSystemRegistration& left,
            const SimulationSystemRegistration& right
        ) noexcept
        {
            return left.type.hash < right.type.hash ||
                   (left.type.hash == right.type.hash && left.type.name < right.type.name);
        }

        [[nodiscard]] SimulationSystemRegistrationFailure registrationFailure(
            ESimulationSystemRegistrationError code,
            lux::system::SystemTypeId type = {}
        ) noexcept
        {
            return SimulationSystemRegistrationFailure{code, std::move(type)};
        }

        [[nodiscard]] bool validAccess(const SystemAccessSpec& access) noexcept
        {
            for (const auto& component : access.components)
            {
                const bool invalid_mode = component.mode != ESystemAccessMode::READ &&
                    component.mode != ESystemAccessMode::WRITE;
                if (!component.type.isValid() || component.storage == 0U || invalid_mode)
                {
                    return false;
                }
            }
            for (const auto& external : access.external)
            {
                const bool invalid_mode = external.mode != ESystemAccessMode::READ &&
                    external.mode != ESystemAccessMode::WRITE;
                if (!external.type.isValid() || invalid_mode)
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool validRegistration(const SimulationSystemRegistration& registration) noexcept
        {
            if (!registration.type.valid() || !registration.cpp_type.isValid() || registration.description == nullptr ||
                registration.install == nullptr || !validSimulationSystemDescription(*registration.description) ||
                !validAccess(registration.access))
            {
                return false;
            }
            const auto canonical_name = registration.description->type.canonical_name;
            if (registration.type.name != canonical_name ||
                registration.type.hash != lux::cxx::Fnv1a64::hash(canonical_name))
            {
                return false;
            }
            const bool has_configuration = !registration.description->type.configuration_schema_name.empty();
            return has_configuration == registration.configuration.valid();
        }
    } // namespace

    SimulationSystemRegistry::SimulationSystemRegistry() : impl_(std::make_unique<Impl>())
    {
    }

    SimulationSystemRegistry::~SimulationSystemRegistry() = default;
    SimulationSystemRegistry::SimulationSystemRegistry(SimulationSystemRegistry&&) noexcept = default;
    SimulationSystemRegistry& SimulationSystemRegistry::operator=(SimulationSystemRegistry&&) noexcept = default;

    lux::cxx::expected<void, SimulationSystemRegistrationFailure>
    SimulationSystemRegistry::add(SimulationSystemRegistration registration) noexcept
    {
        return add(std::span<const SimulationSystemRegistration>(&registration, 1U));
    }

    lux::cxx::expected<void, SimulationSystemRegistrationFailure>
    SimulationSystemRegistry::add(std::span<const SimulationSystemRegistration> registrations) noexcept
    {
        for (const auto& registration : registrations)
        {
            if (!validRegistration(registration))
            {
                return lux::cxx::unexpected(
                    registrationFailure(
                        ESimulationSystemRegistrationError::INVALID_REGISTRATION,
                        registration.type
                    )
                );
            }
        }

        try
        {
            std::vector<SimulationSystemRegistration> combined = impl_->registrations;
            combined.insert(combined.end(), registrations.begin(), registrations.end());
            std::sort(combined.begin(), combined.end(), lessRegistration);
            for (std::size_t index{1U}; index < combined.size(); ++index)
            {
                const auto& previous = combined[index - 1U];
                const auto& current = combined[index];
                if (previous.type.hash == current.type.hash && previous.type.name != current.type.name)
                {
                    return lux::cxx::unexpected(
                        registrationFailure(ESimulationSystemRegistrationError::TYPE_COLLISION, current.type)
                    );
                }
                if (previous.type == current.type)
                {
                    return lux::cxx::unexpected(
                        registrationFailure(ESimulationSystemRegistrationError::DUPLICATE_TYPE, current.type)
                    );
                }
            }

            impl_->registrations = std::move(combined);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                registrationFailure(ESimulationSystemRegistrationError::ALLOCATION_FAILURE)
            );
        }
    }

    const SimulationSystemRegistration* SimulationSystemRegistry::find(const lux::system::SystemTypeId& type) const noexcept
    {
        if (!impl_ || !type.valid())
            return nullptr;
        const auto iterator = std::lower_bound(
            impl_->registrations.begin(),
            impl_->registrations.end(),
            SimulationSystemRegistration{.type = type},
            lessRegistration
        );
        return iterator != impl_->registrations.end() && iterator->type == type ? &*iterator : nullptr;
    }

    std::span<const SimulationSystemRegistration> SimulationSystemRegistry::all() const noexcept
    {
        return impl_ ? std::span<const SimulationSystemRegistration>(impl_->registrations) :
                       std::span<const SimulationSystemRegistration>{};
    }

    std::size_t SimulationSystemRegistry::size() const noexcept
    {
        return impl_ ? impl_->registrations.size() : 0U;
    }
} // namespace lux::simulation
