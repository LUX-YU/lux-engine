#pragma once

#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/features/shadow/ShadowInputs.hpp>
#include <lux/engine/render/resources/lighting/Binding.hpp>
#include <lux/engine/render/resources/instance/Data.hpp>

namespace lux::render::detail
{
    struct ShadowCamera final
    {
        TransformMatrix inverse_view_projection;
        std::array<float, 4> position_near, direction_far;
        bool operator==(const ShadowCamera&) const noexcept = default;
    };

    struct ShadowLightInput final
    {
        VDirectLight value;
        std::uint32_t dense_index = 0;
        bool active = false;
    };

    struct ShadowLightSelection final
    {
        LightId light;
        EDirectLightingBuffer kind = EDirectLightingBuffer::DIRECTIONAL;
        std::vector<float> cascade_far_distances{25};
        float near_distance = 0.05F;
        float caster_distance = 100;
        float normal_offset = 0;
        float comparison_offset = 0;
        float depth_bias_constant = 0, depth_bias_slope = 0;
    };

    struct ShadowProjectionInput final
    {
        ShadowMapShape shape;
        std::span<const ShadowLightSelection> casters;
        std::uint32_t pcf_radius = 1;
    };

    // Pure projection math. Storage/Runtime/GPU ownership stays outside this calculation.
    [[nodiscard]] RenderResult<void> projectShadows(
        const ShadowProjectionInput&,
        const ShadowCamera&,
        std::span<const ShadowLightInput>,
        std::span<MeshVisibilityView>,
        std::span<ShadowSliceGpu>,
        std::span<LightShadowRange>,
        const std::array<std::uint32_t, 3>& offsets
    ) noexcept;

    struct ShadowProjectionSettings final
    {
        ShadowMapShape shape;
        float distance;
        std::uint32_t pcf_radius, maximum_layers;
    };

    // Owns the selected projection independently of any graph, device or upload window.
    class ShadowProjectionData final
    {
    public:
        explicit ShadowProjectionData(ShadowProjectionSettings) noexcept;
        [[nodiscard]] RenderResult<bool> select(const ShadowCamera&, const ShadowLightSnapshot&) noexcept;

        [[nodiscard]] std::span<const MeshVisibilityView> views() const noexcept { return views_; }
        [[nodiscard]] std::span<const ShadowSliceGpu> slices() const noexcept { return slices_; }
        [[nodiscard]] std::span<const RasterState> rasterStates() const noexcept { return raster_states_; }
        [[nodiscard]] std::span<const std::byte> mappings() const noexcept { return mappings_; }
        [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    private:
        ShadowProjectionSettings settings_;
        ShadowCamera camera_{};
        std::uint64_t applied_light_revision_ = 0, revision_ = 0;
        std::vector<ShadowLightSelection> casters_;
        std::vector<ShadowLightInput> selected_lights_;
        std::vector<MeshVisibilityView> views_;
        std::vector<ShadowSliceGpu> slices_;
        std::vector<RasterState> raster_states_;
        std::vector<LightShadowRange> ranges_;
        std::vector<std::byte> mappings_;
    };
} // namespace lux::render::detail
