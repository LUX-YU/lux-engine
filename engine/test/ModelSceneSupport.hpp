#pragma once

#include <filesystem>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/capabilities/Lighting3dCapability.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/resource/asset/storage/directory/DirectoryAssetProvider.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::engine::test
{
    using namespace lux;
    namespace render = ::lux::render;
    namespace rendering = lux::engine::render;
    using Result = render::RenderResult<void>;

    [[nodiscard]] Result runTask(exec::task<Result> task, bool window_events = true) noexcept;
    [[nodiscard]] exec::task<render::RenderResult<toolchain::ModelCookProduct>>
    cookModelAssets(const std::filesystem::path& source, const std::filesystem::path& directory) noexcept;
    [[nodiscard]] exec::task<render::RenderResult<std::shared_ptr<asset::DirectoryAssetProvider>>>
    openModelAssets(const std::filesystem::path& directory) noexcept;
    [[nodiscard]] exec::task<Result>
    resolveComposition(
        render::RenderClient& client,
        render::CapabilityComposition& effect,
        render::EffectOutputDesc output,
        const render::DeferredLightingConfiguration& lighting = {.shadows = false, .clustered = false}
    ) noexcept;
    [[nodiscard]] render::RenderResult<ecs::Entity> createViewComponents(
        ecs::Registry& registry,
        window::LuxWindow& window,
        const render::TargetDesc& description,
        bool orthographic
    ) noexcept;

    class LightCollector final : public rendering::RenderDataCollector
    {
    public:
        static constexpr auto capability = cxx::make_basic_type_info<render::Lighting3dCapability>();
        using CreateResult = render::RenderResult<std::unique_ptr<LightCollector>>;

        static CreateResult create(
            rendering::RenderSystem&,
            render::FrameDataDestination destination,
            std::vector<render::DirectLightUpdate>& lights
        ) noexcept
        {
            return std::unique_ptr<LightCollector>{new LightCollector(lights, destination)};
        }

        Result collect(render::FrameWriter& frame) noexcept override
        {
            auto values = frame.copy<render::DirectLightUpdate>(lights_);
            return values ? frame.write(output_, {0, *values}) : Result{cxx::unexpected(values.error())};
        }

    private:
        LightCollector(
            std::vector<render::DirectLightUpdate>& lights,
            render::FrameDataDestination destination
        ) noexcept
            : lights_(lights), output_(render::frameDataBinding<render::Lighting3dCapability, render::LightUpdates>(
                                   destination.effect,
                                   destination.capability,
                                   destination.scope
                               ))
        {
        }

        std::vector<render::DirectLightUpdate>& lights_;
        render::FrameDataBinding<render::LightUpdates> output_;
    };

} // namespace lux::engine::test
