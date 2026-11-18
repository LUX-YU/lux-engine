#pragma once

#include <lux/engine/render/extensions/graph/InstanceData.hpp>
#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/features/shadow/ShadowProjection.hpp>

namespace lux::render::detail
{
    class RenderGraphInstance;

    class ShadowProjectionBinding final : public InstanceData
    {
    public:
        struct Description final
        {
            ShadowMapFeatureConfig config;
            std::array<GraphResourceId, 4> outputs;
            GraphResourceId lights;
        };

        [[nodiscard]] static RenderResult<std::unique_ptr<InstanceData>>
        create(
            DeviceSession&,
            ResourceRegistry&,
            InstanceAccess&,
            std::span<const ResourceInstanceId>,
            const void*
        ) noexcept;
        ~ShadowProjectionBinding() noexcept;

    private:
        ShadowProjectionBinding(
            DeviceSession&,
            RenderGraphInstance&,
            ResourceRegistry&,
            ResourceInstanceId,
            const Description&
        ) noexcept;
        [[nodiscard]] RenderResult<bool>
        prepare(const InstanceInputs&, ResourceUploader&, detail::Preparation&) noexcept override;

        struct Slice final
        {
            std::array<nri::Buffer*, 3> buffers{};
            std::uint64_t revision = 0;
            std::uint64_t pending_revision = 0;
            bool binding_changed = true;
        };

        DeviceSession& device_;
        RenderGraphInstance& instance_;
        ResourceRegistry& resources_;
        ResourceInstanceId lights_;
        const Description& description_;
        ShadowCamera camera_{};
        std::uint64_t applied_shadow_revision_ = 0, revision_ = 0;
        std::vector<ShadowLightSelection> casters_;
        std::vector<ShadowLightInput> selected_lights_;
        std::vector<MeshVisibilityView> views_;
        std::vector<ShadowSliceGpu> slices_;
        std::vector<RasterState> raster_states_;
        std::vector<std::byte> mappings_;
        std::array<ResourceUpdate, 4> updates_;
        std::array<Slice, 3> gpu_;
    };
} // namespace lux::render::detail
