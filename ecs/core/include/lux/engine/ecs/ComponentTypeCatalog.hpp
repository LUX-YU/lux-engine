#pragma once
/**
 * @file ComponentTypeCatalog.hpp
 * @brief Main-thread catalogue of reflected ECS component schemas.
 *
 * Generated reflection code never mutates a live catalogue. It appends an
 * owning descriptor to a process pending list while meta_module_init/drain is
 * running; the composition root then publishes that batch explicitly through
 * registerGeneratedComponents(). Dynamic extension registration uses the same
 * ComponentSchemaRegistrar transaction surface.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/ecs/ComponentSchemaId.hpp>
#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/visibility.h>
#include <lux/engine/ecs/Registry.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lux::meta { struct RefClass; }

namespace lux::ecs
{
    /// Deterministic default wire identity for reflected components. C++
    /// namespace separators become dots and ASCII letters become lower-case,
    /// so implementation spelling never leaks an invalid `::` name into
    /// LXSC/LXES. Explicit provider schemas may still supply another
    /// canonical stable name in their descriptor transaction.
    [[nodiscard]] LUX_ECS_PUBLIC std::string
    defaultComponentSchemaName(std::string_view cpp_name);


    struct ComponentOperations final
    {
        bool  (*has)(lux::ecs::RegistryBase&, Entity){nullptr};
        void* (*get)(lux::ecs::RegistryBase&, Entity){nullptr};
        void* (*emplace)(lux::ecs::RegistryBase&, Entity){nullptr};
        void  (*remove)(lux::ecs::RegistryBase&, Entity){nullptr};
        void* (*clone)(
            lux::ecs::RegistryBase&,
            Entity,
            lux::ecs::RegistryBase&,
            Entity){nullptr};
        void (*notify)(lux::ecs::RegistryBase&, Entity){nullptr};
        // Reserves EnTT packed/payload capacity for this many additional
        // components. EnTT does not preallocate every sparse entity-index
        // page here, so this is not by itself a zero-heap publication proof.
        // Providers must add the argument to the storage's current size.
        void (*reserve)(
            lux::ecs::RegistryBase&,
            std::size_t){nullptr};
        void* (*transfer)(
            lux::ecs::RegistryBase&,
            Entity,
            lux::ecs::RegistryBase&,
            Entity) noexcept {nullptr};
        bool no_throw_transfer{false};
    };

    enum class EComponentSerializationPolicy : std::uint8_t
    {
        COOKED,
        TRANSIENT,
        PERSISTENT
    };

    struct ComponentSchemaDescriptor final
    {
        // Non-persistent C++ identity. The name storage belongs either to the
        // built-in image or to the ModuleLease retained by lifetime.
        lux::cxx::TypeToken cpp_type{};
        ComponentSchemaId schema_id{};
        std::uint32_t schema_version{1u};
        const lux::meta::RefClass* ref_class{nullptr};
        ComponentOperations operations{};

        // Canonical reverse-domain extension id. Built-ins use
        // "org.lux.builtin". Kept as owned data so DLL string storage never
        // escapes a registration transaction.
        std::string provider;

        // Type-erased ModuleLease. ecs::core deliberately does not depend on
        // engine::extensions; the shared control block still pins code and
        // metadata until the descriptor catalogue is destroyed.
        std::shared_ptr<const void> lifetime;
        EComponentSerializationPolicy serialization{
            EComponentSerializationPolicy::COOKED};

        [[nodiscard]] std::string_view fullName() const noexcept
        {
            return schema_id.name;
        }

        [[nodiscard]] bool has(lux::ecs::RegistryBase& registry,
                               Entity entity) const noexcept
        {
            return operations.has && operations.has(registry, entity);
        }
    };

    enum class EComponentCatalogError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        DUPLICATE_CPP_TYPE,
        DUPLICATE_SCHEMA_NAME,
        CPP_TYPE_HASH_COLLISION,
        SCHEMA_HASH_COLLISION,
        MISSING_SCHEMA,
    };

    struct ComponentCatalogFailure final
    {
        EComponentCatalogError error{
            EComponentCatalogError::INVALID_DESCRIPTOR
        };
        std::string name;
        std::string conflicting_name;
    };

    template <typename T>
    using ComponentCatalogExp = lux::cxx::expected<T, ComponentCatalogFailure>;

    class LUX_ECS_PUBLIC ComponentTypeCatalog final
    {
    public:
        using RegisterSchemaResult = ComponentCatalogExp<const ComponentSchemaDescriptor*>;
        using RegisterSchemasResult = ComponentCatalogExp<std::size_t>;
        using ValidationResult = ComponentCatalogExp<void>;

        ComponentTypeCatalog() = default;
        ComponentTypeCatalog(const ComponentTypeCatalog&) = delete;
        ComponentTypeCatalog& operator=(const ComponentTypeCatalog&) = delete;

        // Descriptor addresses and all() spans remain valid until the next
        // successful registration. A rejected registration is a strict
        // transaction: it does not relocate entries or change either lookup
        // index.
        [[nodiscard]] RegisterSchemaResult registerSchema(ComponentSchemaDescriptor descriptor);

        [[nodiscard]] ValidationResult validateSchemas(std::span<const ComponentSchemaDescriptor> descriptors) const;

        [[nodiscard]] RegisterSchemasResult registerSchemas(std::span<const ComponentSchemaDescriptor> descriptors);

        [[nodiscard]] const ComponentSchemaDescriptor* findBySchema(std::string_view name) const noexcept;

        /// C++ reflection/tooling lookup. Cooked EntityScene paths must use
        /// findBySchema() and never persist this implementation identity.
        [[nodiscard]] const ComponentSchemaDescriptor* findByCppName(std::string_view name) const noexcept;

        [[nodiscard]] const ComponentSchemaDescriptor* findByType(
            lux::cxx::TypeToken type) const noexcept;

        [[nodiscard]] std::span<const ComponentSchemaDescriptor> all() const noexcept;

    private:
        [[nodiscard]] lux::cxx::expected<void, ComponentCatalogFailure> validate(
            const ComponentSchemaDescriptor& descriptor,
            std::span<const ComponentSchemaDescriptor> pending = {}
        ) const;

        std::vector<ComponentSchemaDescriptor>          entries_;
        std::unordered_map<std::uint64_t, std::size_t>  schema_index_;
        std::unordered_map<std::uint64_t, std::size_t>  type_index_;
    };

    /// Called only by generated reflection callbacks. This does not publish a
    /// component to any consumer; it appends to the main-thread pending chain.
    LUX_ECS_PUBLIC void queueGeneratedComponent(ComponentSchemaDescriptor descriptor) noexcept;

    /// Publish every descriptor queued by the most recent meta_module_init or
    /// meta_module_drain. On validation failure the pending chain is retained
    /// and the destination catalogue remains unchanged.
    [[nodiscard]] LUX_ECS_PUBLIC ComponentCatalogExp<std::size_t>
    registerGeneratedComponents(ComponentTypeCatalog& catalog);

    /// Move the descriptors produced by the latest meta-module drain into a
    /// caller-owned registration transaction. Dynamic extension loading uses
    /// this path so generated schemas are validated and published atomically
    /// with the module's other contributions.
    [[nodiscard]] LUX_ECS_PUBLIC
    std::vector<ComponentSchemaDescriptor> takeGeneratedComponents() noexcept;

    /// Pack declaration drift check. Diagnostics stay at composition call
    /// sites; this helper has no terminal-I/O side effects.
    using ComponentCatalogValidationResult = ComponentCatalogExp<void>;

    [[nodiscard]] LUX_ECS_PUBLIC ComponentCatalogValidationResult validateComponentSchemas(
        const ComponentTypeCatalog& catalog,
        std::span<const std::string_view> schema_names
    );
}
