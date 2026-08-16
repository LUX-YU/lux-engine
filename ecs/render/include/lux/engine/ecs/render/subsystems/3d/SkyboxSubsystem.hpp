#pragma once

#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include <lux/engine/ecs/render/RenderBridgeDiagnostics.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>
#include <lux/engine/ecs/render/components/TextureGpuCacheComponent.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace lux::ecs
{
    /// Bridges the scene's optional singleton SkyboxComponent. Unlike the
    /// generic feature-parameter shape, this owner explicitly clears the
    /// renderer when the component disappears and rejects ambiguous content.
    class SkyboxSubsystem final : public IRenderSubsystem
    {
    public:
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            static constexpr std::string_view features[]{"Skybox"};
            return features;
        }

        [[nodiscard]] std::span<const RenderSubsystemType>
        runsAfter() const noexcept override
        {
            static constexpr RenderSubsystemType dependencies[]{
                renderSubsystemType<ResidencySubsystem>()};
            return dependencies;
        }

        void update(RenderSubsystemContext& context) override
        {
            auto& registry = context.registry();
            auto& render = context.render();
            const auto feature = render.features().handle("Skybox");
            const auto operations = render.features().ops<
                lux::render::SkyboxOperationIds>("Skybox");
            if (!feature.isValid() || !operations.valid())
                return;

            lux::meta::entity_id selected = lux::meta::null_entity;
            std::size_t count = 0u;
            for (const auto entity : registry.view<SkyboxComponent>())
            {
                selected = entity;
                ++count;
            }

            if (count > 1u)
            {
                if (!duplicate_reported_)
                {
                    diagnoseRenderBridge(
                        "[SkyboxSubsystem] expected at most one "
                        "SkyboxComponent, found %zu",
                        count);
                    duplicate_reported_ = true;
                }
                clear(render, operations, feature);
                return;
            }
            duplicate_reported_ = false;

            if (count == 0u)
            {
                clear(render, operations, feature);
                return;
            }

            const auto& component = registry.get<SkyboxComponent>(selected);
            if (!std::isfinite(component.rotation_radians) ||
                !std::isfinite(component.intensity) ||
                component.intensity < 0.0f)
            {
                if (!invalid_reported_)
                {
                    diagnoseRenderBridge(
                        "[SkyboxSubsystem] rejected non-finite or negative "
                        "SkyboxComponent parameters");
                    invalid_reported_ = true;
                }
                return;
            }
            invalid_reported_ = false;

            if (component.equirect_texture_id.is_nil())
            {
                clear(render, operations, feature);
                return;
            }

            const auto* texture =
                registry.try_get<TextureGpuCacheComponent>(selected);
            if (!texture || texture->handle.isNull() ||
                texture->source != component.equirect_texture_id)
            {
                // Residency keeps the previous binding alive until the new
                // asset is ready. Preserve the last valid renderer state too.
                return;
            }

            const State next{
                texture->handle,
                component.rotation_radians,
                component.intensity};
            if (last_ && *last_ == next)
                return;

            lux::render::SkyboxSetEquirectPayload payload{};
            payload.scene_id = render.scene();
            payload.feature = feature;
            payload.texture = next.texture;
            payload.rotation_radians = next.rotation_radians;
            payload.intensity = next.intensity;
            lux::render::SkyboxProxy{render.session(), operations}
                .setEquirect(payload);
            last_ = next;
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        void close(RenderSubsystemContext& context) noexcept override
        {
            const auto feature = context.render().features().handle("Skybox");
            const auto operations = context.render().features().ops<
                lux::render::SkyboxOperationIds>("Skybox");
            if (feature.isValid() && operations.valid())
                clear(context.render(), operations, feature);
        }

        void closeScene(RenderSubsystemContext&) noexcept override
        {
            // DestroyScene owns renderer-side skybox state and needs no final
            // Frame-lane command after the lexical frame has closed.
            last_.reset();
        }

    private:
        struct State final
        {
            lux::render::RTextureHandle texture{};
            float rotation_radians{0.0f};
            float intensity{1.0f};

            friend bool operator==(const State&, const State&) = default;
        };

        void clear(
            SceneRenderBinding& render,
            const lux::render::SkyboxOperationIds& operations,
            lux::render::FeatureHandle feature)
        {
            if (!last_)
                return;
            lux::render::SkyboxSetEquirectPayload payload{};
            payload.scene_id = render.scene();
            payload.feature = feature;
            lux::render::SkyboxProxy{render.session(), operations}
                .setEquirect(payload);
            last_.reset();
        }

        std::optional<State> last_;
        bool duplicate_reported_{false};
        bool invalid_reported_{false};
    };
}
