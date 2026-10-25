#pragma once

#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/extensions/graph/Work.hpp>
#include <lux/engine/render/features/ShadowProjection.hpp>

namespace lux::render::detail
{
    class ShadowProjectionBinding final
    {
    public:
        struct Description final
        {
            ShadowMapFeatureConfig config;
            std::array<GraphResourceId, 4> outputs;
            GraphResourceId lights;
        };

        [[nodiscard]] static RenderResult<FeatureBinding>
        create(DeviceSession&, FrameExecution&, ResourceRegistry&, std::span<const StorageId>, const void*) noexcept;
        ~ShadowProjectionBinding() noexcept;

    private:
        ShadowProjectionBinding(DeviceSession&, FrameExecution&, ResourceRegistry&, StorageId, const Description&) noexcept;
        [[nodiscard]] RenderResult<FeatureBinding::Preparation>
        prepareFrame(const FeatureFrameInput&, ResourceUploader&) noexcept;

        struct Slice final
        {
            std::array<nri::Buffer*, 3> buffers{};
            std::uint64_t revision = 0;
            std::size_t uploaded = 0, pending = 0;
            std::uint64_t pending_revision = 0;
            bool binding_changed = true;
        };

        DeviceSession& device_;
        FrameExecution& frames_;
        ResourceRegistry& resources_;
        StorageId lights_;
        const Description& description_;
        ShadowCamera camera_{};
        std::uint64_t light_revision_ = 0, revision_ = 0;
        std::vector<ShadowLightInput> selected_lights_;
        std::vector<MeshVisibilityView> views_;
        std::vector<ShadowSliceGpu> slices_;
        std::vector<RasterState> raster_states_;
        rdesc::ShaderBlock view_layout_;
        std::vector<std::byte> mappings_;
        std::array<ResourceUpdate, 4> updates_;
        std::array<Slice, 3> gpu_;
    };
} // namespace lux::render::detail
