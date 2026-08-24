#pragma once

#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/RenderBridgeDiagnostics.hpp>
#include <lux/engine/ecs/render/components/3d/HeightFogComponent.hpp>
#include <lux/engine/function/render/client/genops/FogOperation.ops.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>

namespace lux::ecs
{
    /// Bridges an optional singleton HeightFogComponent to FogFeature. The
    /// ECS component is authoritative; WaterFeature may consume FogFeature's
    /// renderer-local state without creating a second ECS-to-render operation.
    class HeightFogSubsystem final : public RenderStage
    {
    public:
        [[nodiscard]] std::span<const std::string_view>
        requiredFeatures() const noexcept override
        {
            static constexpr std::string_view features[]{"Fog"};
            return features;
        }

        void extract(RenderExtractContext& context) override
        {
            auto& registry = context.registry();
            lux::ecs::Entity selected = lux::ecs::kNullEntity;
            std::size_t count = 0u;
            for (const auto entity : registry.view<HeightFogComponent>())
            {
                selected = entity;
                ++count;
            }

            if (count > 1u)
            {
                if (!duplicate_reported_)
                {
                    diagnoseRenderBridge(
                        "[HeightFogSubsystem] expected at most one "
                        "HeightFogComponent, found {}",
                        count);
                    duplicate_reported_ = true;
                }
                clear(context.render());
                return;
            }
            duplicate_reported_ = false;

            if (count == 0u)
            {
                clear(context.render());
                return;
            }

            const auto& component =
                registry.get<HeightFogComponent>(selected);
            const auto next = stateOf(component);
            if (!next)
            {
                if (!invalid_reported_)
                {
                    diagnoseRenderBridge(
                        "[HeightFogSubsystem] rejected invalid "
                        "HeightFogComponent parameters");
                    invalid_reported_ = true;
                }
                return;
            }
            invalid_reported_ = false;
            apply(context.render(), *next);
        }

    private:
        struct State final
        {
            std::array<float, 3u> color{0.55f, 0.62f, 0.70f};
            float density{0.0002f};
            float start_distance{0.0f};
            float reference_height{0.0f};
            float height_falloff{0.01f};
            float maximum_opacity{0.98f};
            bool enabled{false};

            friend bool operator==(const State&, const State&) = default;
        };

        [[nodiscard]] static std::optional<State> stateOf(
            const HeightFogComponent& component) noexcept
        {
            if (!component.color.allFinite() ||
                !std::isfinite(component.density) ||
                !std::isfinite(component.start_distance) ||
                !std::isfinite(component.reference_height) ||
                !std::isfinite(component.height_falloff) ||
                !std::isfinite(component.maximum_opacity) ||
                component.density < 0.0f ||
                component.start_distance < 0.0f ||
                component.height_falloff < 0.0f ||
                component.maximum_opacity < 0.0f ||
                component.maximum_opacity > 1.0f)
            {
                return std::nullopt;
            }

            return State{
                {component.color.x(), component.color.y(),
                 component.color.z()},
                component.density,
                component.start_distance,
                component.reference_height,
                component.height_falloff,
                component.maximum_opacity,
                component.enabled};
        }

        void apply(SceneRenderBinding& render, const State& state)
        {
            const auto fog_feature = render.features().handle("Fog");
            const auto fog_operations = render.features().ops<
                lux::render::FogOperationIds>("Fog");
            if (fog_feature.isValid() && fog_operations.valid() &&
                (!last_fog_ || *last_fog_ != state))
            {
                lux::render::FogSetParamsPayload payload{};
                payload.scene_id = render.scene();
                payload.feature = fog_feature;
                std::ranges::copy(state.color, payload.color);
                payload.density = state.density;
                payload.start_distance = state.start_distance;
                payload.reference_height = state.reference_height;
                payload.height_falloff = state.height_falloff;
                payload.maximum_opacity = state.maximum_opacity;
                payload.enabled = state.enabled ? 1u : 0u;
                lux::render::FogProxy{render.session(), fog_operations}
                    .setParams(payload);
                last_fog_ = state;
            }

        }

        void clear(SceneRenderBinding& render)
        {
            if (!last_fog_)
                return;
            apply(render, State{});
            last_fog_.reset();
        }

        std::optional<State> last_fog_;
        bool duplicate_reported_{false};
        bool invalid_reported_{false};
    };
}
