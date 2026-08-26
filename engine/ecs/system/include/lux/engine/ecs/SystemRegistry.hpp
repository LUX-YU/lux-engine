#pragma once

#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace lux::ecs
{
    namespace detail
    {
        template <class Type>
        struct SystemControl final
        {
            template <class... Args>
            explicit SystemControl(
                std::shared_ptr<const void> lifetime,
                Args&&... args
            )
                : code_lifetime(std::move(lifetime)),
                  object(std::forward<Args>(args)...)
            {
            }

            // Reverse member destruction keeps code loaded through ~Type().
            std::shared_ptr<const void> code_lifetime;
            Type object;
        };
    }

    /**
     * Owner-thread composition container. Erase invalidates membership while
     * retained typed leases keep the object and its code alive.
     */
    class LUX_ENGINE_ECS_SYSTEM_PUBLIC SystemRegistry final
    {
      public:
        SystemRegistry();
        ~SystemRegistry();
        SystemRegistry(SystemRegistry&& other) noexcept;
        SystemRegistry& operator=(SystemRegistry&& other) noexcept;
        SystemRegistry(const SystemRegistry&) = delete;
        SystemRegistry& operator=(const SystemRegistry&) = delete;

        template <class Type, class... Args>
        [[nodiscard]] lux::cxx::expected<SystemId, SystemFailure> emplace(
            Args&&... args
        ) noexcept
        {
            return emplaceWithLifetime<Type>(
                {},
                std::forward<Args>(args)...
            );
        }

        template <class Type, class... Args>
        [[nodiscard]] lux::cxx::expected<SystemId, SystemFailure>
        emplaceWithLifetime(
            std::shared_ptr<const void> code_lifetime,
            Args&&... args
        ) noexcept
        {
            try
            {
                using Control = detail::SystemControl<Type>;
                auto control = std::make_shared<Control>(
                    std::move(code_lifetime),
                    std::forward<Args>(args)...
                );
                std::shared_ptr<void> object(control, &control->object);
                return add(lux::cxx::typeToken<Type>(), std::move(object));
            }
            catch (...)
            {
                return lux::cxx::unexpected(SystemFailure{
                    .code = ESystemError::ALLOCATION_FAILURE
                });
            }
        }

        template <class Type>
        [[nodiscard]] std::shared_ptr<Type> retain(SystemId id) noexcept
        {
            auto object = retainErased(id, lux::cxx::typeToken<Type>());
            if (!object)
                return {};
            auto* value = static_cast<Type*>(object.get());
            return std::shared_ptr<Type>(std::move(object), value);
        }

        [[nodiscard]] SystemRegistryId id() const noexcept;
        [[nodiscard]] bool contains(SystemId id) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] bool erase(SystemId id) noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        [[nodiscard]] lux::cxx::expected<SystemId, SystemFailure> add(
            lux::cxx::TypeToken type,
            std::shared_ptr<void> object
        ) noexcept;
        [[nodiscard]] std::shared_ptr<void> retainErased(
            SystemId id,
            lux::cxx::TypeToken type
        ) noexcept;
    };
}
