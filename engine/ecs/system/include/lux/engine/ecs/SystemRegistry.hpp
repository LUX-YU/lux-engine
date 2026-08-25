#pragma once

#include <lux/engine/ecs/SystemConcept.hpp>
#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/SystemStart.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    class SystemContext;

    namespace detail
    {
        struct SystemRecord final
        {
            lux::cxx::TypeToken type;
            SystemAccessSpec access;
            std::shared_ptr<const void> code_lifetime;
            std::shared_ptr<void> object;
            void (*update)(void*, SystemContext&) noexcept{};
            lux::cxx::expected<void, SystemStartError> (*start)(
                void*,
                SystemStart&
            ) noexcept{};
            void (*request_stop)(void*) noexcept{};
            bool (*stopped)(const void*) noexcept{};
            bool (*affinity_valid)(const void*) noexcept{};
            bool owner_thread_affine{};
            std::uint64_t registration_order{};
            bool started{};
        };

        template <class Type>
        concept ThreadAffineSystem = requires(const Type& system)
        {
            typename Type::lux_thread_affine;
            requires std::same_as<
                typename Type::lux_thread_affine,
                std::true_type
            >;
            { system.isOnAffinityThread() } noexcept -> std::same_as<bool>;
        };

        template <class Type>
        [[nodiscard]] SystemRecord eraseSystem(
            std::unique_ptr<Type> object,
            std::shared_ptr<const void> code_lifetime
        )
        {
            SystemRecord record;
            record.type = lux::cxx::typeToken<Type>();
            record.access = Type::Access;
            record.code_lifetime = std::move(code_lifetime);
            record.object = std::shared_ptr<void>(
                object.release(),
                [](void* value) noexcept
                {
                    delete static_cast<Type*>(value);
                }
            );
            record.update = [](void* value, SystemContext& context) noexcept
            {
                static_cast<Type*>(value)->update(context);
            };
            record.start = [](void* value, SystemStart& context) noexcept
                -> lux::cxx::expected<void, SystemStartError>
            {
                if constexpr (requires(Type& system, SystemStart& start)
                {
                    { system.start(start) } noexcept -> std::same_as<
                        lux::cxx::expected<void, SystemStartError>
                    >;
                })
                {
                    return static_cast<Type*>(value)->start(context);
                }
                else
                    return {};
            };
            record.request_stop = [](void* value) noexcept
            {
                if constexpr (requires(Type& system)
                {
                    { system.requestStop() } noexcept -> std::same_as<void>;
                })
                {
                    static_cast<Type*>(value)->requestStop();
                }
            };
            record.stopped = [](const void* value) noexcept
            {
                if constexpr (requires(const Type& system)
                {
                    { system.stopped() } noexcept -> std::same_as<bool>;
                })
                {
                    return static_cast<const Type*>(value)->stopped();
                }
                else
                    return true;
            };
            record.affinity_valid = [](const void* value) noexcept
            {
                if constexpr (ThreadAffineSystem<Type>)
                    return static_cast<const Type*>(value)->isOnAffinityThread();
                else
                    return true;
            };
            record.owner_thread_affine = ThreadAffineSystem<Type>;
            return record;
        }

        struct SystemRegistryAccess;
    }

    class LUX_ENGINE_ECS_SYSTEM_PUBLIC SystemRegistry final
    {
    public:
        SystemRegistry();
        ~SystemRegistry();

        SystemRegistry(SystemRegistry&&) noexcept;
        SystemRegistry& operator=(SystemRegistry&&) noexcept;

        SystemRegistry(const SystemRegistry&) = delete;
        SystemRegistry& operator=(const SystemRegistry&) = delete;

        template <System Type, class... Args>
        [[nodiscard]] lux::cxx::expected<SystemId, SystemFailure> emplace(
            Args&&... args
        ) noexcept
        {
            return emplaceWithLifetime<Type>(
                {},
                std::forward<Args>(args)...
            );
        }

        template <System Type, class... Args>
        [[nodiscard]] lux::cxx::expected<SystemId, SystemFailure>
        emplaceWithLifetime(
            std::shared_ptr<const void> code_lifetime,
            Args&&... args
        ) noexcept
        {
            try
            {
                auto object = std::make_unique<Type>(
                    std::forward<Args>(args)...
                );
                auto record = detail::eraseSystem(
                    std::move(object),
                    std::move(code_lifetime)
                );
                if (!record.affinity_valid(record.object.get()))
                {
                    return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                        .code = ESystemError::EXECUTION_AFFINITY_MISMATCH
                    });
                }
                return add(std::move(record));
            }
            catch (...)
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::ALLOCATION_FAILURE
                });
            }
        }

        [[nodiscard]] bool contains(SystemId id) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;

        [[nodiscard]] bool erase(SystemId id) noexcept;
        [[nodiscard]] bool requestStop(SystemId id) noexcept;
        [[nodiscard]] bool stopped(SystemId id) const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        [[nodiscard]] lux::cxx::expected<SystemId, SystemFailure> add(
            detail::SystemRecord record
        ) noexcept;

        friend struct detail::SystemRegistryAccess;
    };
}
