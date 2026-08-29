#include <lux/engine/simulation/SystemRegistry.hpp>

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

namespace lux::simulation
{
    struct SystemRegistry::Impl final
    {
        std::vector<SystemRegistration> registrations;
    };

    namespace
    {
        [[nodiscard]] bool lessRegistration(
            const SystemRegistration& left,
            const SystemRegistration& right
        ) noexcept
        {
            return left.type.hash < right.type.hash ||
                   (left.type.hash == right.type.hash && left.type.name < right.type.name);
        }

        [[nodiscard]] SystemRegistrationFailure registrationFailure(
            ESystemRegistrationError code,
            SystemTypeId type = {}
        ) noexcept
        {
            return SystemRegistrationFailure{code, std::move(type)};
        }
    } // namespace

    SystemRegistry::SystemRegistry() : impl_(std::make_unique<Impl>())
    {
    }

    SystemRegistry::~SystemRegistry() = default;
    SystemRegistry::SystemRegistry(SystemRegistry&&) noexcept = default;
    SystemRegistry& SystemRegistry::operator=(SystemRegistry&&) noexcept = default;

    lux::cxx::expected<void, SystemRegistrationFailure>
    SystemRegistry::add(SystemRegistration registration) noexcept
    {
        return add(std::span<const SystemRegistration>(&registration, 1U));
    }

    lux::cxx::expected<void, SystemRegistrationFailure>
    SystemRegistry::add(std::span<const SystemRegistration> registrations) noexcept
    {
        for (const auto& registration : registrations)
        {
            if (!registration.type.valid() || registration.version == 0U || registration.install == nullptr)
            {
                return lux::cxx::unexpected(
                    registrationFailure(
                        ESystemRegistrationError::INVALID_REGISTRATION,
                        registration.type
                    )
                );
            }
        }

        try
        {
            std::vector<SystemRegistration> combined = impl_->registrations;
            combined.insert(combined.end(), registrations.begin(), registrations.end());
            std::sort(combined.begin(), combined.end(), lessRegistration);
            for (std::size_t index{1U}; index < combined.size(); ++index)
            {
                const auto& previous = combined[index - 1U];
                const auto& current = combined[index];
                if (previous.type.hash == current.type.hash && previous.type.name != current.type.name)
                {
                    return lux::cxx::unexpected(
                        registrationFailure(ESystemRegistrationError::TYPE_COLLISION, current.type)
                    );
                }
                if (previous.type == current.type)
                {
                    return lux::cxx::unexpected(
                        registrationFailure(ESystemRegistrationError::DUPLICATE_TYPE, current.type)
                    );
                }
            }

            impl_->registrations = std::move(combined);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                registrationFailure(ESystemRegistrationError::ALLOCATION_FAILURE)
            );
        }
    }

    const SystemRegistration* SystemRegistry::find(const SystemTypeId& type) const noexcept
    {
        if (!impl_ || !type.valid())
            return nullptr;
        const auto iterator = std::lower_bound(
            impl_->registrations.begin(),
            impl_->registrations.end(),
            SystemRegistration{type, 0U, nullptr},
            lessRegistration
        );
        return iterator != impl_->registrations.end() && iterator->type == type ? &*iterator : nullptr;
    }

    std::size_t SystemRegistry::size() const noexcept
    {
        return impl_ ? impl_->registrations.size() : 0U;
    }
} // namespace lux::simulation
