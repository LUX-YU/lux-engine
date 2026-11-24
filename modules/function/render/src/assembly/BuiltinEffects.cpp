#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/effect/capabilities/DeferredGeometry.hpp>
#include <lux/engine/render/effect/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/effect/capabilities/DepthReduction.hpp>
#include <lux/engine/render/effect/capabilities/ForwardGeometry.hpp>
#include <lux/engine/render/effect/capabilities/MeshVisibility.hpp>
#include <lux/engine/render/effect/capabilities/ShadowRendering.hpp>
#include <lux/engine/render/effect/capabilities/ToneMapping.hpp>
#include <lux/engine/render/resource/instance/MeshInstanceStorage.hpp>
#include <lux/engine/render/resource/lighting/DirectLightingStorage.hpp>
#include <lux/engine/render/resource/material/MaterialStorage.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>
#include <lux/engine/render/resource/transform/TransformStorage.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <optional>

namespace lux::render
{
    RenderResult<void> RenderServer::registerBuiltins() noexcept
    {
        if (auto view = registerResource(detail::sceneViewRegistration()); !view)
            return cxx::unexpected(view.error());
        const std::array registrations{
            std::pair{&detail::SampledTexture2DStorage::factory, &detail::sampledTexture2DStorageCommands},
            std::pair{&detail::IndexedTriangleMeshStorage::factory, &detail::indexedTriangleMeshStorageCommands},
            std::pair{&detail::MaterialStorage::factory, &detail::materialCommands},
            std::pair{&detail::MeshInstanceStorage::factory, &detail::meshInstanceCommands},
            std::pair{&detail::TransformStorage::factory, &detail::transformStorageCommands},
            std::pair{&detail::DirectLightingStorage::factory, &detail::directLightingStorageCommands}
        };
        for (const auto& [factory, commands] : registrations)
        {
            auto added = registerResource(factory(), {}, commands());
            if (!added) return cxx::unexpected(added.error());
        }
        const auto install = [this](
            const EffectCapability& capability, auto implementations, std::optional<CommandBinding> setter = {}
        ) -> RenderResult<void>
        {
            std::vector<CommandBindingLocation> locations;
            std::vector<CommandId> commands;
            if (setter)
            {
                const auto command = registerNodeEffectCommand(*setter);
                if (!command) return cxx::unexpected(command.error());
                commands.push_back(command->command);
                locations.push_back(*command);
            }
            if (auto added = registerEffectCapability(capability, std::move(commands)); !added) return added;
            for (auto& implementation : implementations)
            {
                if (auto added = registerEffectImplementation(std::move(implementation), locations); !added) return added;
            }
            return {};
        };
        using namespace detail;
        if (auto depth = install(capabilities::DepthReduction, std::array{depthPyramidImplementation()}); !depth)
            return depth;
        if (auto light = install(capabilities::DeferredLighting, lightingImplementations(), lightingEnvironmentBinding()); !light)
            return light;
        if (auto visibility = install(capabilities::MeshVisibility, std::array{meshVisibilityImplementation()}); !visibility)
            return visibility;
        if (auto geometry = install(capabilities::DeferredGeometry, gBufferImplementations()); !geometry) return geometry;
        if (auto forward = install(capabilities::ForwardGeometry, meshRasterImplementations()); !forward) return forward;
        if (auto shadow = install(capabilities::ShadowRendering, shadowImplementations()); !shadow) return shadow;
        return install(capabilities::ToneMapping, toneMappingImplementations(), toneMappingExposureBinding());
    }
}
