#pragma once

#include <lux/engine/render/effect/implementations/mesh_visibility/VisibilityInputs.hpp>
#include <lux/engine/render/graph/Work.hpp>
#include <lux/engine/render/effect/implementations/shadow/ProjectionInputs.hpp>
#include <lux/engine/render/resource/lighting/Data.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>
#include <lux/engine/render/effect/EffectFrameWriter.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>

namespace lux::render::detail
{
    struct ShadowProjectionSettings final
    {
        ShadowMapShape shape;
        float distance;
        std::uint32_t pcf_radius, maximum_layers;
    };

    // Each installed Shadow algorithm retains its own projections until its inputs change.
    class ShadowProjectionCache final
    {
    public:
        explicit ShadowProjectionCache(ShadowProjectionSettings settings) noexcept : settings_(settings) {}
        [[nodiscard]] RenderResult<void> select(const ViewGpuParameters&, ResourceViewValue<ShadowLightSnapshot>) noexcept;

        [[nodiscard]] FrameBufferUpload viewUpload() const & noexcept
        {
            return {std::as_bytes(std::span{views_}), view_revision_};
        }
        [[nodiscard]] FrameBufferUpload sliceUpload() const & noexcept
        {
            return {std::as_bytes(std::span{slices_}), slice_revision_};
        }
        [[nodiscard]] FrameBufferUpload mappingUpload() const & noexcept { return {mappings_, mapping_revision_}; }
        [[nodiscard]] std::span<const RasterState> rasterStates() const & noexcept { return raster_states_; }

    private:
        [[nodiscard]] RenderResult<bool>
        project(const ViewGpuParameters&, const ShadowLightSnapshot&, bool lights_changed) noexcept;

        ShadowProjectionSettings settings_;
        TransformMatrix inverse_view_projection_{};
        std::array<float, 4> position_near_{}, direction_far_{};
        std::uint64_t applied_light_revision_ = 0;
        std::uint64_t view_revision_ = 0, slice_revision_ = 0, mapping_revision_ = 0;
        std::vector<MeshVisibilityView> views_;
        std::vector<ShadowSliceGpu> slices_;
        std::vector<RasterState> raster_states_;
        std::vector<std::byte> mappings_;
    };
} // namespace lux::render::detail
