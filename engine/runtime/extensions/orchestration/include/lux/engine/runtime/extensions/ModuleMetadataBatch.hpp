#pragma once
/**
 * @file ModuleMetadataBatch.hpp
 * @brief One validate-before-publish metadata batch produced by a module load.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/runtime/extensions/orchestration_visibility.h>

#include <cstdint>
#include <vector>

namespace lux::extensions
{
    enum class EModuleMetadataBatchError : std::uint8_t
    {
        INVALID_MODULE,
        REFLECTION_VALIDATION_FAILED,
        COMPONENT_VALIDATION_FAILED,
    };

    struct ModuleMetadataBatchFailure final
    {
        EModuleMetadataBatchError code{
            EModuleMetadataBatchError::INVALID_MODULE};
        std::uint32_t detail{0u};
    };

    /// A small transaction object for exactly one module-load event. Reflection
    /// and Component metadata remain separate registries; only their validation
    /// and publication event is shared.
    class LUX_RUNTIME_EXTENSION_ORCHESTRATION_PUBLIC ModuleMetadataBatch final
    {
    public:
        ModuleMetadataBatch() noexcept = default;
        ModuleMetadataBatch(const ModuleMetadataBatch&) = delete;
        ModuleMetadataBatch& operator=(const ModuleMetadataBatch&) = delete;
        ModuleMetadataBatch(ModuleMetadataBatch&&) noexcept = default;
        ModuleMetadataBatch& operator=(ModuleMetadataBatch&&) noexcept =
            default;

        [[nodiscard]] static lux::cxx::expected<
            ModuleMetadataBatch,
            ModuleMetadataBatchFailure>
        prepare(
            ModuleLease module,
            lux::ecs::ComponentTypeCatalog& components) noexcept;

        /// prepare() performed every recoverable check. Owner-thread
        /// confinement makes publication an invariant-only, no-fail step.
        void commit() noexcept;

        [[nodiscard]] std::size_t componentCount() const noexcept
        {
            return components_.size();
        }

    private:
        ModuleLease module_;
        lux::meta::ReflectionRegistrationDraft reflection_;
        std::vector<lux::ecs::ComponentSchemaDescriptor> components_;
        lux::ecs::ComponentTypeCatalog* component_catalog_{nullptr};
        bool prepared_{false};
        bool committed_{false};
    };
}
