#include <lux/engine/runtime/render/scene/BuiltinRenderEffects.hpp>

#include <lux/engine/ecs/render/subsystems/3d/Grid3DSubsystem.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>

#include <memory>
#include <span>
#include <utility>

namespace lux::runtime
{
    lux::cxx::expected<void, ERenderEffectCatalogError>
    addGrid3DRenderEffect(RenderEffectCatalog& catalog)
    {
        lux::render::Grid3DCommConfig config{};
        RenderEffectDescriptor descriptor;
        descriptor.id = lux::render::RenderEffectId{
            "org.lux.render.grid3d.effect"};
        descriptor.display_name = "Grid 3D";
        descriptor.factory = lux::render::kGrid3DFeatureFactory;
        descriptor.config_schema_version = 1u;
        descriptor.default_config = ContributionConfig{
            1u,
            lux::cxx::SharedBytes<>::copyOf(
                std::as_bytes(std::span{&config, 1u}))};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.builtin"};
        descriptor.build_extraction = [](
            lux::ecs::RenderSubsystemMutationBatch& batch,
            const RenderEffectBuildContext&,
            ContributionConfig)
            -> lux::cxx::expected<void, RenderExtractionBuildFailure>
        {
            auto added = batch.add(
                std::make_unique<lux::ecs::Grid3DSubsystem>());
            if (!added)
            {
                return lux::cxx::unexpected(
                    RenderExtractionBuildFailure{
                        ERenderExtractionBuildError::DUPLICATE_SUBSYSTEM,
                        added.error().subject});
            }
            return {};
        };
        return catalog.add(std::move(descriptor));
    }
}
