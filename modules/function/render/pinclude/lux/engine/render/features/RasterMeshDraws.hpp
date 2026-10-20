#pragma once

#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>

namespace lux::render::detail
{
    struct MeshDrawConfiguration final
    {
        GraphResourceId instances;
        MeshInstanceDrawGroup selection;
    };

    // One Attachment's derived selection and indirect commands. RasterMeshInstanceStorage only reports
    // identity changes; selection, grouping, allocation and recording belong to this Native Feature.
    class RasterMeshDraws final
    {
    public:
        [[nodiscard]] static RenderResult<FeatureFrameWork>
        create(DeviceSession&, ExecutionContext&, GraphResourceId, const void*) noexcept;
        ~RasterMeshDraws() noexcept;

    private:
        RasterMeshDraws(DeviceSession&, ExecutionContext&, GraphResourceId, MeshInstanceDrawGroup) noexcept;
        [[nodiscard]] RenderResult<FeatureFrameWork::EPreparation> prepare(std::uint32_t, UploadContext&) noexcept;
        void record(VkCommandBuffer, std::uint32_t) noexcept;
        void update(MeshInstanceId, const MeshInstanceDescription*) noexcept;
        [[nodiscard]] bool selected(const MeshInstanceDescription&) const noexcept;
        DeviceSession& session_;
        ExecutionContext& context_;
        GraphResourceId resource_;
        StorageResource sections_;
        MeshInstanceDrawGroup selection_;
        RasterMeshInstanceStorage::Observer observer_;

        struct GpuSlice final
        {
            gapi::vk::VmaBuffer buffer;
            std::uint64_t uploaded_revision = 0, pending_revision = 0;
            std::size_t uploaded_bytes = 0, pending_bytes = 0;
        };

        std::array<GpuSlice, 3> gpu_;
        std::uint64_t revision_ = 1;
        std::vector<std::uint32_t> words_, positions_;
        bool available_ = true;
    };
} // namespace lux::render::detail
