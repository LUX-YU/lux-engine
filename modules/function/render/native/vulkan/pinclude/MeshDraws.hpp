#pragma once

#include <lux/engine/render/features/mesh/MeshFeature.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>

namespace lux::render::vulkan
{
    struct MeshDrawConfiguration final
    {
        GraphResourceId instances;
        MeshInstanceDrawGroup selection;
    };

    // One Attachment's derived selection and indirect commands. MeshInstanceStorage only reports
    // identity changes; selection, grouping, allocation and recording belong to this Native Feature.
    class MeshDraws final
    {
    public:
        [[nodiscard]] static RenderResult<ContextData>
        create(DeviceSession&, ExecutionContext&, GraphResourceId, const void*) noexcept;
        ~MeshDraws() noexcept;

    private:
        MeshDraws(DeviceSession&, ExecutionContext&, GraphResourceId, MeshInstanceDrawGroup) noexcept;
        [[nodiscard]] RenderResult<ContextData::EPreparation> prepare(std::uint32_t, UploadContext&) noexcept;
        void record(VkCommandBuffer, std::uint32_t) noexcept;
        void update(MeshInstanceId, const MeshInstanceDescription*) noexcept;
        [[nodiscard]] bool selected(const MeshInstanceDescription&) const noexcept;
        DeviceSession& session_;
        ExecutionContext& context_;
        GraphResourceId resource_;
        StorageResource sections_;
        MeshInstanceDrawGroup selection_;
        MeshInstanceStorage::Observer observer_;

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
} // namespace lux::render::vulkan
