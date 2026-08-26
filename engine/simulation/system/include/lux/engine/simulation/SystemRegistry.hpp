#pragma once

#include <lux/engine/simulation/SystemConcept.hpp>
#include <lux/engine/simulation/SystemError.hpp>
#include <lux/engine/simulation/system/visibility.h>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace lux::simulation
{
    namespace detail
    {
        struct SystemRecord final
        {
            ~SystemRecord() noexcept
            {
                if (object != nullptr)
                    destroy(object);
            }

            SystemRecord() = default;
            SystemRecord(const SystemRecord&) = delete;
            SystemRecord& operator=(const SystemRecord&) = delete;

            lux::cxx::TypeToken type;
            std::shared_ptr<const void> code_lifetime;
            void* object{};
            void (*destroy)(void*) noexcept{};
        };
    }

    /** Strong lifetime lease suitable for capture by a TaskGraph callable. */
    template <class Type>
    class SystemLease final
    {
    public:
        SystemLease() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return record_ != nullptr && object_ != nullptr;
        }

        [[nodiscard]] Type& get() const noexcept
        {
            return *object_;
        }

        [[nodiscard]] Type* operator->() const noexcept
        {
            return object_;
        }

        [[nodiscard]] Type& operator*() const noexcept
        {
            return *object_;
        }

        [[nodiscard]] SystemId id() const noexcept
        {
            return id_;
        }

    private:
        SystemLease(
            SystemId id,
            std::shared_ptr<detail::SystemRecord> record,
            Type* object
        ) noexcept
            : id_(id), record_(std::move(record)), object_(object)
        {
        }

        SystemId id_{};
        std::shared_ptr<detail::SystemRecord> record_;
        Type* object_{};

        friend class SystemRegistry;
    };

    /**
     * Heterogeneous System object owner only. It does not compile schedules and it
     * does not execute systems. TaskGraph lambdas capture SystemLease<T> values.
     */
    class LUX_ENGINE_SIMULATION_SYSTEM_PUBLIC SystemRegistry final
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
            return emplaceWithLifetime<Type>({}, std::forward<Args>(args)...);
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
                auto object = std::make_unique<Type>(std::forward<Args>(args)...);
                auto record = std::make_shared<detail::SystemRecord>();
                record->type = lux::cxx::typeToken<Type>();
                record->code_lifetime = std::move(code_lifetime);
                record->object = object.release();
                record->destroy = [](void* value) noexcept
                {
                    delete static_cast<Type*>(value);
                };
                return add(std::move(record));
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(SystemFailure{
                    .code = ESystemError::ALLOCATION_FAILURE
                });
            }
            catch (...)
            {
                return lux::cxx::unexpected(SystemFailure{
                    .code = ESystemError::CONSTRUCTION_FAILURE
                });
            }
        }

        template <System Type>
        [[nodiscard]] lux::cxx::expected<SystemLease<Type>, SystemFailure>
        retain(SystemId id) const noexcept
        {
            auto record = retainRecord(id);
            if (!record)
                return lux::cxx::unexpected(record.error());

            const auto expected_type = lux::cxx::typeToken<Type>();
            if ((*record)->type.hash() != expected_type.hash())
            {
                return lux::cxx::unexpected(SystemFailure{
                    .code = ESystemError::INVALID_SYSTEM,
                    .system = id
                });
            }
            if ((*record)->type.name() != expected_type.name())
            {
                return lux::cxx::unexpected(SystemFailure{
                    .code = ESystemError::TYPE_COLLISION,
                    .system = id
                });
            }
            return SystemLease<Type>(
                id,
                *record,
                static_cast<Type*>((*record)->object)
            );
        }

        [[nodiscard]] SystemRegistryId id() const noexcept;
        [[nodiscard]] bool contains(SystemId id) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;

        /**
         * Remove registry membership immediately. Physical destruction is deferred
         * until all SystemLease values (including compiled TaskGraph captures) die.
         */
        [[nodiscard]] bool erase(SystemId id) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        [[nodiscard]] lux::cxx::expected<SystemId, SystemFailure> add(
            std::shared_ptr<detail::SystemRecord> record
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<
            std::shared_ptr<detail::SystemRecord>,
            SystemFailure
        > retainRecord(SystemId id) const noexcept;
    };
}
