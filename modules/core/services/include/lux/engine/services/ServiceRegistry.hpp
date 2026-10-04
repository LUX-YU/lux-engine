#pragma once

#include <functional>
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/services/ServiceDescriptor.hpp>
#include <vector>

namespace lux::services
{
    namespace detail
    {
        struct ServiceDefinition;
        struct ServiceScopeState;
    } // namespace detail
    class ServiceRegistry;
    class LUX_SERVICES_PUBLIC ServiceHandle final
    {
    public:
        ServiceHandle() noexcept = default;
        [[nodiscard]] bool valid() const noexcept
        {
            return bool(definition_);
        }
        [[nodiscard]] const ServiceDescriptor& descriptor() const noexcept;

    private:
        friend class ServiceRegistry;
        std::shared_ptr<const detail::ServiceDefinition> definition_;
        std::size_t contract_{};
    };
    // A lexical scope handle; instances can outlive its logical close. The Registry and borrowed
    // infrastructure must remain until drained() succeeds, including dispatcher reclamation.
    class LUX_SERVICES_PUBLIC ServiceScope final
    {
    public:
        ~ServiceScope();
        ServiceScope(ServiceScope&&) noexcept;
        ServiceScope& operator=(ServiceScope&&) = delete;
        ServiceScope(const ServiceScope&) = delete;
        ServiceScope& operator=(const ServiceScope&) = delete;
        [[nodiscard]] ServiceResult<void> beginClose() noexcept;
        [[nodiscard]] ServiceResult<void> cancelClose() noexcept;
        // Call only after the domain's close prerequisites have been accepted. No business is run here.
        [[nodiscard]] ServiceResult<void> release() noexcept;
        [[nodiscard]] bool drained() const noexcept;
        template <class T>
        [[nodiscard]] ServiceResult<void> provide(
            ServiceNameView contract,
            T& value,
            std::uint32_t version = 1,
            std::string_view qualifier = {}
        ) noexcept
        {
            return provide(contract, cxx::typeToken<T>(), &value, version, qualifier);
        }

    private:
        friend class ServiceRegistry;
        ServiceScope(ServiceRegistry&, std::shared_ptr<detail::ServiceScopeState>) noexcept;
        [[nodiscard]] ServiceResult<void> provide(
            ServiceNameView,
            cxx::TypeToken,
            void*,
            std::uint32_t,
            std::string_view
        ) noexcept;
        ServiceRegistry* registry_;
        std::shared_ptr<detail::ServiceScopeState> state_;
    };
    class LUX_SERVICES_PUBLIC ServiceResolver final
    {
    public:
        ServiceResolver(const ServiceResolver&) = delete;
        ServiceResolver& operator=(const ServiceResolver&) = delete;
        ServiceResolver(ServiceResolver&&) = delete;
        ServiceResolver& operator=(ServiceResolver&&) = delete;
        template <class T>
        [[nodiscard]] ServiceResult<std::shared_ptr<T>> get(
            std::size_t dependency,
            const ServiceConfiguration& configuration = {}
        ) noexcept
        {
            auto result = get(dependency, cxx::typeToken<T>(), configuration);
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            return std::static_pointer_cast<T>(std::move(*result));
        }
        template <class T>
        [[nodiscard]] ServiceResult<std::reference_wrapper<T>> require(std::size_t dependency) noexcept
        {
            auto result = require(dependency, cxx::typeToken<T>());
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            return std::ref(*static_cast<T*>(*result));
        }
        [[nodiscard]] const object::ObjectDispatcherRef& dispatcher() const noexcept;
        [[nodiscard]] bool isOpen() const noexcept;
        template <class T>
        [[nodiscard]] ServiceResult<std::shared_ptr<const T>> definition() const noexcept
        {
            if (!entry_)
            {
                return cxx::unexpected(ServiceFailure{EServiceError::UNDECLARED_DEPENDENCY});
            }
            return entry_->definition<T>();
        }

    private:
        friend class ServiceRegistry;
        ServiceResolver(
            ServiceRegistry&,
            std::span<const ServiceDependency>,
            std::shared_ptr<detail::ServiceScopeState>,
            const ServiceEntry* = nullptr
        ) noexcept;
        [[nodiscard]] ServiceResult<std::shared_ptr<void>>
        get(std::size_t, cxx::TypeToken, const ServiceConfiguration&) noexcept;
        [[nodiscard]] ServiceResult<void*> require(std::size_t, cxx::TypeToken) noexcept;
        ServiceRegistry& registry_;
        std::span<const ServiceDependency> dependencies_;
        std::shared_ptr<detail::ServiceScopeState> scope_;
        const ServiceEntry* entry_;
    };
    struct ServiceLimits final
    {
        std::size_t definitions{128};
        std::size_t scopes{256};
        std::size_t instances{1024};
        std::size_t borrowed_per_scope{64};
    };
    class LUX_SERVICES_PUBLIC ServiceRegistry final
    {
    public:
        explicit ServiceRegistry(object::ObjectDispatcherRef, ServiceLimits = {});
        ~ServiceRegistry();
        ServiceRegistry(const ServiceRegistry&) = delete;
        ServiceRegistry& operator=(const ServiceRegistry&) = delete;
        ServiceRegistry(ServiceRegistry&&) = delete;
        ServiceRegistry& operator=(ServiceRegistry&&) = delete;
        // A short multi-catalog read scope. Lazy instances remain available, but the definition
        // set cannot change until all fixed-snapshot factory calls and their cleanup have returned.
        class LUX_SERVICES_PUBLIC ReadScope final
        {
        public:
            ~ReadScope();
            ReadScope(ReadScope&&) noexcept;
            ReadScope(const ReadScope&) = delete;
            ReadScope& operator=(const ReadScope&) = delete;
            ReadScope& operator=(ReadScope&&) = delete;

        private:
            friend class ServiceRegistry;
            explicit ReadScope(ServiceRegistry&) noexcept;
            ServiceRegistry* owner_;
        };
        [[nodiscard]] ServiceResult<ReadScope> readScope() noexcept;
        // A short owner-thread publication scope. It spans validation, all participating swaps,
        // retired-descriptor cleanup and notifications; never keep it across an asynchronous operation.
        class LUX_SERVICES_PUBLIC Publication final
        {
        public:
            ~Publication();
            Publication(Publication&&) noexcept;
            Publication& operator=(Publication&&) = delete;
            Publication(const Publication&) = delete;
            Publication& operator=(const Publication&) = delete;
            // One prepared swap, without allocation or callbacks. Old code remains protected until destruction.
            void commit() noexcept;
            // Release the abandoned candidate or retired definitions while every participant is
            // still guarded. This consumes an uncommitted permission; the guard remains until return.
            void clearRetained() noexcept;

        private:
            friend class ServiceRegistry;
            struct State;
            explicit Publication(std::unique_ptr<State>) noexcept;
            std::unique_ptr<State> state_;
        };
        [[nodiscard]] ServiceResult<
            Publication> preparePublication(std::vector<std::shared_ptr<const ServiceEntry>>) noexcept;
        // Factory-only lexical dependency access. The caller pins its declaration throughout the call.
        // The resolver cannot escape, publish catalogs, or discover undeclared dependencies.
        [[nodiscard]] ServiceResult<void>
        withDependencies(ServiceScope&, std::span<const ServiceDependency>, cxx::function_ref<ServiceResult<void>(ServiceResolver&)>) noexcept;
        // Validates a complete candidate. Existing handles/instances keep their original definition generation.
        [[nodiscard]] ServiceResult<void> publish(std::vector<std::shared_ptr<const ServiceEntry>>) noexcept;
        [[nodiscard]] ServiceResult<ServiceScope> createScope(const ServiceScope* parent = nullptr) noexcept;
        template <class T>
        [[nodiscard]] ServiceResult<ServiceHandle> resolve(
            ServiceNameView contract = {},
            std::uint32_t version = 1,
            ServiceNameView implementation = {}
        ) const noexcept
        {
            return resolve(cxx::typeToken<T>(), contract, version, implementation);
        }
        template <class T>
        [[nodiscard]] ServiceResult<std::shared_ptr<T>> get(
            const ServiceHandle& handle,
            ServiceScope& scope,
            std::string_view qualifier = {},
            const ServiceConfiguration& configuration = {}
        ) noexcept
        {
            auto result = get(handle, scope, cxx::typeToken<T>(), qualifier, configuration);
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            return std::static_pointer_cast<T>(std::move(*result));
        }
        template <class T>
        [[nodiscard]] ServiceResult<std::shared_ptr<T>> get(
            ServiceScope& scope,
            std::string_view qualifier = {},
            const ServiceConfiguration& configuration = {}
        ) noexcept
        {
            auto handle = resolve<T>();
            if (!handle)
            {
                return cxx::unexpected(std::move(handle.error()));
            }
            return get<T>(*handle, scope, qualifier, configuration);
        }
        // Physical liveness, not use_count and not task completion. No collection or business callbacks.
        [[nodiscard]] bool drained() const noexcept;

    private:
        friend class ServiceScope;
        friend class ServiceResolver;
        [[nodiscard]] ServiceResult<ServiceHandle> resolve(
            cxx::TypeToken,
            ServiceNameView,
            std::uint32_t,
            ServiceNameView
        ) const noexcept;
        [[nodiscard]] ServiceResult<std::shared_ptr<void>>
        get(const ServiceHandle&,
            ServiceScope&,
            cxx::TypeToken,
            std::string_view,
            const ServiceConfiguration&) noexcept;
        [[nodiscard]] ServiceResult<std::shared_ptr<void>>
        instantiate(ServiceHandle, std::shared_ptr<detail::ServiceScopeState>, std::string_view, const ServiceConfiguration&) noexcept;
        [[nodiscard]] ServiceResult<std::shared_ptr<detail::ServiceScopeState>>
        dependencyScope(const ServiceResolver&, const ServiceDependency&) const noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::services
