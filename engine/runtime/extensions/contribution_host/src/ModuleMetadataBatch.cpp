#include <lux/engine/runtime/extensions/ModuleMetadataBatch.hpp>

#include <exception>
#include <utility>

namespace lux::extensions
{
    lux::cxx::expected<ModuleMetadataBatch, ModuleMetadataBatchFailure>
    ModuleMetadataBatch::prepare(
        ModuleLease module,
        lux::ecs::ComponentTypeCatalog& components) noexcept
    {
        if (!module)
        {
            return lux::cxx::unexpected(ModuleMetadataBatchFailure{
                EModuleMetadataBatchError::INVALID_MODULE});
        }

        auto reflection = lux::meta::meta_module_drain_draft();
        auto component_descriptors = lux::ecs::takeGeneratedComponents();
        if (!reflection)
        {
            return lux::cxx::unexpected(ModuleMetadataBatchFailure{
                EModuleMetadataBatchError::REFLECTION_VALIDATION_FAILED});
        }

        for (auto& descriptor : component_descriptors)
        {
            descriptor.provider = module->id().name();
            descriptor.lifetime = module;
        }
        if (auto validated = components.validateSchemas(
                component_descriptors);
            !validated)
        {
            return lux::cxx::unexpected(ModuleMetadataBatchFailure{
                EModuleMetadataBatchError::COMPONENT_VALIDATION_FAILED,
                static_cast<std::uint32_t>(validated.error().error)});
        }

        ModuleMetadataBatch batch;
        batch.module_ = std::move(module);
        batch.reflection_ = std::move(reflection);
        batch.components_ = std::move(component_descriptors);
        batch.component_catalog_ = &components;
        batch.prepared_ = true;
        return batch;
    }

    void ModuleMetadataBatch::commit() noexcept
    {
        if (!prepared_ || committed_ || !component_catalog_)
            std::terminate();

        auto reflection = reflection_.commit();
        if (!reflection)
            std::terminate();
        auto components = component_catalog_->registerSchemas(components_);
        if (!components)
            std::terminate();

        committed_ = true;
        components_.clear();
    }
}
