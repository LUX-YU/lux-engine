#pragma once

#include <concepts>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/services/visibility.h>
#include <span>
#include <string>

namespace lux::object
{
    class LuxObject;
}

namespace lux::services
{
    struct ServiceNameTag;
    using ServiceName = cxx::StableNameId<ServiceNameTag>;
    using ServiceNameView = cxx::StableNameIdView<ServiceNameTag>;

    enum class EServiceError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        INVALID_CONFIGURATION,
        INVALID_SCOPE,
        WRONG_THREAD,
        BUSY,
        CLOSED,
        CAPACITY,
        NOT_FOUND,
        AMBIGUOUS_PROVIDER,
        VERSION_MISMATCH,
        TYPE_MISMATCH,
        CONFIGURATION_MISMATCH,
        UNDECLARED_DEPENDENCY,
        DEPENDENCY_CYCLE,
        INVALID_SCOPE_DEPENDENCY,
        DUPLICATE,
        HASH_COLLISION,
        FACTORY_FAILURE,
        RETIRING
    };
    struct ServiceFailure final
    {
        EServiceError code;
        std::string detail;
        std::string domain;
        std::uint64_t domain_code{};
    };
    template <class T> using ServiceResult = cxx::expected<T, ServiceFailure>;

    enum class EServiceRetention : std::uint8_t
    {
        SHARED,
        SCOPED
    };
    enum class EServiceAffinity : std::uint8_t
    {
        NONE,
        OWNER
    };
    enum class EDependencyKind : std::uint8_t
    {
        SHARED,
        BORROWED,
        DEFINITIONS
    };
    enum class EDependencyScope : std::uint8_t
    {
        SAME,
        PARENT,
        ROOT
    };

    struct ServiceConfiguration final
    {
        ServiceName schema;
        std::uint32_t version{};
        cxx::TypeToken type;
        cxx::SharedBytes<> bytes;
    };
    struct ServiceSchema final
    {
        ServiceNameView id;
        std::uint32_t version{};
        cxx::TypeToken type;
        ServiceResult<void> (*validate)(const ServiceConfiguration&) noexcept {};
    };
    struct ServiceContract final
    {
        ServiceNameView id;
        std::uint32_t version{1};
        cxx::TypeToken type;
        // All contracts project from the single allocation returned by create().
        void* (*project)(void*) noexcept {};
        template <class Implementation, class Contract>
        [[nodiscard]] static constexpr ServiceContract forType(ServiceNameView id, std::uint32_t version = 1)
        {
            return {
                id,
                version,
                cxx::typeToken<Contract>(),
                [](void* value) noexcept -> void*
                { return static_cast<Contract*>(static_cast<Implementation*>(value)); }
            };
        }
    };
    struct ServiceDependency final
    {
        ServiceNameView contract;
        std::uint32_t version{1};
        cxx::TypeToken type;
        EDependencyKind kind{EDependencyKind::SHARED};
        EDependencyScope scope{EDependencyScope::SAME};
        ServiceNameView implementation;
        std::string_view qualifier;
        bool optional{};
        [[nodiscard]] bool isValid() const noexcept
        {
            const bool has_identity = contract.isValid() && version != 0 && type.isValid();
            const bool has_valid_policy = kind <= EDependencyKind::DEFINITIONS && scope <= EDependencyScope::ROOT;
            const bool is_invalid_definition_scope = kind == EDependencyKind::DEFINITIONS &&
                                                     (!qualifier.empty() || scope != EDependencyScope::SAME);
            return has_identity && has_valid_policy && !is_invalid_definition_scope;
        }
    };
    class ServiceResolver;
    // Static declarations reference module constants. Dynamic declarations freeze this same shape once.
    struct ServiceDescriptor final
    {
        ServiceNameView implementation;
        std::uint32_t version{1};
        cxx::TypeToken allocation_type;
        std::span<const ServiceContract> contracts;
        std::span<const ServiceDependency> dependencies;
        ServiceSchema configuration;
        EServiceRetention retention{EServiceRetention::SHARED};
        EServiceAffinity affinity{EServiceAffinity::NONE};
        ServiceResult<void*> (*create)(ServiceResolver&, const ServiceConfiguration&) noexcept {};
        void (*destroy)(void*) noexcept {};
        // Required for LuxObject allocations so the existing callback/retirement protection applies.
        object::LuxObject* (*object)(void*) noexcept {};
        // Immutable declaration input, distinct from per-instance serialized configuration. The entry
        // owns its backing and code; the factory may retain it without retaining the resolver.
        cxx::TypeToken definition_type;
        // Optional fact query for accepted work. No cancellation, confirmation or destruction occurs here.
        // Domain review/decisions precede release; READY here alone is not permission to discard data.
        ServiceResult<bool> (*settled)(const void*) noexcept {};
        // Owner safe-point work of an already created instance. No factory runs and failures do not
        // skip independent participants. Accepted completion and retirement remain the domain's facts.
        ServiceResult<void> (*maintain)(void*) noexcept {};

        // Declaration-time adaptation only. The runtime invokes one erased boundary with a matching
        // destruction function; modules construct with their actual unique owner and error type.
        template <class T, auto Factory>
            requires requires(ServiceResolver& resolver, const ServiceConfiguration& configuration) {
                { Factory(resolver, configuration) } noexcept -> std::same_as<ServiceResult<std::unique_ptr<T>>>;
            }
        [[nodiscard]] static constexpr ServiceDescriptor forType(
            ServiceNameView implementation,
            std::span<const ServiceContract> contracts,
            std::span<const ServiceDependency> dependencies = {}
        ) noexcept
        {
            ServiceDescriptor result;
            result.implementation = implementation;
            result.allocation_type = cxx::typeToken<T>();
            result.contracts = contracts;
            result.dependencies = dependencies;
            result.create = [](ServiceResolver& resolver,
                               const ServiceConfiguration& configuration) noexcept -> ServiceResult<void*>
            {
                auto candidate = Factory(resolver, configuration);
                if (!candidate)
                {
                    return cxx::unexpected(std::move(candidate.error()));
                }
                return candidate->release();
            };
            result.destroy = [](void* value) noexcept { delete static_cast<T*>(value); };
            if constexpr (std::derived_from<T, object::LuxObject>)
            {
                result.affinity = EServiceAffinity::OWNER;
                result.object = [](void* value) noexcept -> object::LuxObject* { return static_cast<T*>(value); };
            }
            return result;
        }
    };
    class ServiceEntry;
    // Pure declaration validation. No factory, configuration callback or instance is created here.
    [[nodiscard]] LUX_SERVICES_PUBLIC ServiceResult<void> validateServiceEntries(
        std::span<const std::shared_ptr<const ServiceEntry>>,
        std::size_t capacity
    ) noexcept;
    class LUX_SERVICES_PUBLIC ServiceEntry final
    {
    public:
        template <auto& Descriptor>
            requires std::same_as<std::remove_cvref_t<decltype(Descriptor)>, ServiceDescriptor> &&
                     std::is_const_v<std::remove_reference_t<decltype(Descriptor)>>
        [[nodiscard]] static std::shared_ptr<const ServiceEntry> bind(object::CodeLease code)
        {
            auto entry = std::shared_ptr<const ServiceEntry>(new ServiceEntry(code, Descriptor));
            return object::pinCodeOwner(std::move(code), std::move(entry));
        }
        template <auto& Descriptor, class T>
            requires std::same_as<std::remove_cvref_t<decltype(Descriptor)>, ServiceDescriptor> &&
                     std::is_const_v<std::remove_reference_t<decltype(Descriptor)>>
        [[nodiscard]] static std::shared_ptr<const ServiceEntry> bind(
            object::CodeLease code,
            std::shared_ptr<const T> definition
        )
        {
            auto entry = std::shared_ptr<ServiceEntry>(new ServiceEntry(code, Descriptor));
            entry->definition_type_ = cxx::typeToken<T>();
            entry->definition_ = object::pinCodeOwner(code, std::move(definition));
            return object::pinCodeOwner(std::move(code), std::shared_ptr<const ServiceEntry>(std::move(entry)));
        }
        [[nodiscard]] static std::shared_ptr<const ServiceEntry> create(object::CodeLease, const ServiceDescriptor&);
        template <class T>
        [[nodiscard]] static std::shared_ptr<const ServiceEntry> create(
            object::CodeLease code,
            const ServiceDescriptor& descriptor,
            std::shared_ptr<const T> definition
        )
        {
            auto pinned = object::pinCodeOwner(code, std::move(definition));
            return create(std::move(code), descriptor, cxx::typeToken<T>(), std::move(pinned));
        }
        template <class T> [[nodiscard]] ServiceResult<std::shared_ptr<const T>> definition() const noexcept
        {
            auto value = definition(cxx::typeToken<T>());
            if (!value)
            {
                return cxx::unexpected(std::move(value.error()));
            }
            return std::static_pointer_cast<const T>(std::move(*value));
        }
        ~ServiceEntry();
        ServiceEntry(const ServiceEntry&) = delete;
        ServiceEntry& operator=(const ServiceEntry&) = delete;
        ServiceEntry(ServiceEntry&&) = delete;
        ServiceEntry& operator=(ServiceEntry&&) = delete;
        [[nodiscard]] const ServiceDescriptor& descriptor() const noexcept
        {
            return *descriptor_;
        }
        [[nodiscard]] const object::CodeLease& code() const noexcept
        {
            return code_;
        }

    private:
        friend class ServiceRegistry;
        friend class ServiceResolver;
        friend LUX_SERVICES_PUBLIC ServiceResult<void> validateServiceEntries(
            std::span<const std::shared_ptr<const ServiceEntry>>,
            std::size_t
        ) noexcept;
        [[nodiscard]] static std::shared_ptr<const ServiceEntry>
        create(object::CodeLease, const ServiceDescriptor&, cxx::TypeToken, std::shared_ptr<const void>);
        [[nodiscard]] ServiceResult<std::shared_ptr<const void>> definition(cxx::TypeToken) const noexcept;
        ServiceEntry(object::CodeLease, const ServiceDescriptor&);
        struct Storage;
        object::CodeLease code_;
        std::shared_ptr<const void> definition_;
        cxx::TypeToken definition_type_;
        std::unique_ptr<const Storage> storage_;
        const ServiceDescriptor* descriptor_;
    };
} // namespace lux::services
