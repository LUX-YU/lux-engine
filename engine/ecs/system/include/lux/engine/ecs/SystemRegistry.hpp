#pragma once

#include <lux/engine/ecs/SystemConcept.hpp>
#include <lux/engine/ecs/SystemError.hpp>
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
            SystemRecord() = default;

            ~SystemRecord() noexcept
            {
                reset();
            }

            SystemRecord(SystemRecord&& other) noexcept
                : type(other.type),
                  access(other.access),
                  code_lifetime(std::move(other.code_lifetime)),
                  object(std::exchange(other.object, nullptr)),
                  destroy(std::exchange(other.destroy, nullptr)),
                  update(other.update),
                  affinity_valid(other.affinity_valid),
                  owner_thread_affine(other.owner_thread_affine),
                  registration_order(other.registration_order)
            {
            }

            SystemRecord& operator=(SystemRecord&& other) noexcept
            {
                if (this == std::addressof(other))
                    return *this;
                reset();
                type = other.type;
                access = other.access;
                code_lifetime = std::move(other.code_lifetime);
                object = std::exchange(other.object, nullptr);
                destroy = std::exchange(other.destroy, nullptr);
                update = other.update;
                affinity_valid = other.affinity_valid;
                owner_thread_affine = other.owner_thread_affine;
                registration_order = other.registration_order;
                return *this;
            }

            SystemRecord(const SystemRecord&) = delete;
            SystemRecord& operator=(const SystemRecord&) = delete;

            void reset() noexcept
            {
                if (object != nullptr)
                    destroy(object);
                object = nullptr;
                destroy = nullptr;
            }

            lux::cxx::TypeToken type;
            SystemAccessSpec access;
            std::shared_ptr<const void> code_lifetime;
            void* object{};
            void (*destroy)(void*) noexcept{};
            void (*update)(void*, SystemContext&) noexcept{};
            bool (*affinity_valid)(const void*) noexcept{};
            bool owner_thread_affine{};
            std::uint64_t registration_order{};
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
        ) noexcept
        {
            SystemRecord record;
            record.type = lux::cxx::typeToken<Type>();
            record.access = Type::Access.spec();
            record.code_lifetime = std::move(code_lifetime);
            record.object = object.release();
            record.destroy = [](void* value) noexcept
            {
                delete static_cast<Type*>(value);
            };
            record.update = [](void* value, SystemContext& context) noexcept
            {
                static_cast<Type*>(value)->update(context);
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

        SystemRegistry(SystemRegistry&& other) noexcept;
        SystemRegistry& operator=(SystemRegistry&& other) noexcept;

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
                return add(std::move(record));
            }
            catch (...)
            {
                return lux::cxx::unexpected<SystemFailure>(SystemFailure{
                    .code = ESystemError::ALLOCATION_FAILURE
                });
            }
        }

        [[nodiscard]] SystemRegistryId id() const noexcept;
        [[nodiscard]] bool contains(SystemId id) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;

        /** Immediately destroys the uniquely-owned System at a safe point. */
        [[nodiscard]] bool erase(SystemId id) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        [[nodiscard]] lux::cxx::expected<SystemId, SystemFailure> add(
            detail::SystemRecord record
        ) noexcept;

        friend struct detail::SystemRegistryAccess;
    };
}
