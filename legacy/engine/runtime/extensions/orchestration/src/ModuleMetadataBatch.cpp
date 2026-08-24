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

        std::vector<lux::ecs::ComponentSchemaDescriptor>
            component_descriptors;
        lux::meta::ReflectionRegistrationDraft reflection;
        {
            lux::ecs::GeneratedComponentDraftCapture capture{
                component_descriptors};
            reflection = lux::meta::meta_module_drain_draft();
        }
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
        auto component_registration = components.prepareSchemas(
            component_descriptors);
        if (!component_registration)
        {
            return lux::cxx::unexpected(ModuleMetadataBatchFailure{
                EModuleMetadataBatchError::COMPONENT_VALIDATION_FAILED,
                static_cast<std::uint32_t>(
                    component_registration.error().error)});
        }

        ModuleMetadataBatch batch;
        batch.module_ = std::move(module);
        batch.reflection_ = std::move(reflection);
        batch.component_count_ = component_descriptors.size();
        batch.component_registration_.emplace(
            std::move(*component_registration));
        batch.prepared_ = true;
        return batch;
    }

    void ModuleMetadataBatch::commit() noexcept
    {
        if (!prepared_ || committed_ || !component_registration_)
            std::terminate();

        auto reflection = reflection_.commit();
        if (!reflection)
            std::terminate();
        (void)component_registration_->commit();

        committed_ = true;
        component_registration_.reset();
    }
}
