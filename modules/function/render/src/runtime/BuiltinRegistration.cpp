#include <lux/engine/render/effect/builtin/GBuffer.hpp>
#include <lux/engine/render/effect/builtin/MeshRaster.hpp>
#include <lux/engine/render/effect/builtin/MeshVisibility.hpp>
#include <lux/engine/render/effect/builtin/Shadow.hpp>
#include <lux/engine/render/effect/builtin/DepthPyramid.hpp>
#include <lux/engine/render/effect/builtin/Implementations.hpp>
#include <lux/engine/render/effect/builtin/Lighting.hpp>
#include <lux/engine/render/effect/builtin/ToneMapping.hpp>
#include <lux/engine/render/runtime/BuiltinRegistration.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>

namespace lux::render::detail
{
    namespace
    {
        RenderResult<void> registerAdjustableNode(
            RenderServer& server,
            EffectNodeDefinition definition,
            std::array<EffectImplementation, 2> implementations,
            CommandBinding setter
        ) noexcept
        {
            auto command = server.registerNodeEffectCommand(setter);
            if (!command) return cxx::unexpected(command.error());
            auto added = server.registerEffectCapability(std::move(definition), {command->command});
            if (!added) return added;
            for (auto& implementation : implementations)
            {
                added = server.registerEffectImplementation(std::move(implementation), {*command});
                if (!added) return added;
            }
            return {};
        }
    }

    RenderResult<void> registerDepthPyramid(RenderServer& server) noexcept
    {
        auto added = server.registerEffectCapability(DepthPyramid::definition());
        if (!added) return added;
        return server.registerEffectImplementation(depthPyramidImplementation());
    }

    RenderResult<void> registerLighting(RenderServer& server) noexcept
    {
        return registerAdjustableNode(server, Lighting::definition(), lightingImplementations(), lightingEnvironmentBinding());
    }

    RenderResult<void> registerToneMapping(RenderServer& server) noexcept
    {
        return registerAdjustableNode(
            server, ToneMapping::definition(), toneMappingImplementations(), toneMappingExposureBinding()
        );
    }

    RenderResult<void> registerMeshVisibility(RenderServer& server) noexcept
    {
        auto result = server.registerEffectCapability(MeshVisibility::definition());
        if (!result) return result;
        return server.registerEffectImplementation(meshVisibilityImplementation());
    }

    RenderResult<void> registerGBuffer(RenderServer& server) noexcept
    {
        auto result = server.registerEffectCapability(GBuffer::definition());
        if (!result) return result;
        for (auto& implementation : gBufferImplementations())
        {
            result = server.registerEffectImplementation(std::move(implementation));
            if (!result) return result;
        }
        return {};
    }

    RenderResult<void> registerMeshRaster(RenderServer& server) noexcept
    {
        auto result = server.registerEffectCapability(MeshRaster::definition());
        if (!result) return result;
        for (auto& implementation : meshRasterImplementations())
        {
            result = server.registerEffectImplementation(std::move(implementation));
            if (!result) return result;
        }
        return {};
    }

    RenderResult<void> registerShadow(RenderServer& server) noexcept
    {
        auto result = server.registerEffectCapability(Shadow::definition());
        if (!result) return result;
        for (auto& implementation : shadowImplementations())
        {
            result = server.registerEffectImplementation(std::move(implementation));
            if (!result) return result;
        }
        return {};
    }
}
