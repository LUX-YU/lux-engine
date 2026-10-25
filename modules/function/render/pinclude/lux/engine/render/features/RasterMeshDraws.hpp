#pragma once

#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>

namespace lux::render::detail
{
    struct MeshDrawConfiguration final
    {
        GraphResourceId instances;
        MeshInstanceDrawGroup selection;
        GraphResourceId members;
        std::uint32_t capacity = UINT32_MAX;
    };

    // One Attachment's derived selection and indirect commands. RasterMeshInstanceStorage only reports
    // identity changes; selection, grouping, allocation and recording belong to this Native Feature.
    class RasterMeshDraws final
    {
    public:
        [[nodiscard]] static RenderResult<FeatureBinding>
        create(DeviceSession&, FrameExecution&, ResourceRegistry&, std::span<const StorageId>, const void*) noexcept;
        ~RasterMeshDraws() noexcept;

    private:
        RasterMeshDraws(DeviceSession&, FrameExecution&, ResourceRegistry&, MeshInstanceDrawGroup) noexcept;
        [[nodiscard]] RenderResult<FeatureBinding::Preparation> prepare(std::uint32_t, ResourceUploader&) noexcept;
        void update(MeshInstanceId, const RasterMeshInstanceStorage::Selection*) noexcept;
        [[nodiscard]] bool selected(const RasterMeshInstanceStorage::Selection&) const noexcept;
        DeviceSession& session_;
        FrameExecution& frames_;
        ResourceRegistry& resources_;
        ResourceInput sections_;
        MeshInstanceDrawGroup selection_;
        RasterMeshInstanceStorage::Observer observer_;

        struct GpuSlice final
        {
            nri::Buffer* buffer = nullptr;
            std::array<ResourceUploader::BufferCopy, 3> copies;
            bool binding_changed = false;
            std::uint64_t uploaded_revision = 0, pending_revision = 0;
            std::size_t uploaded_bytes = 0, pending_bytes = 0;
        };

        std::array<GpuSlice, 3> gpu_;
        std::uint64_t revision_ = 1;
        std::vector<std::uint32_t> words_, positions_;
        bool available_ = true;
        ResourceUpdate update_;
        std::uint32_t capacity_ = UINT32_MAX;
    };
} // namespace lux::render::detail
