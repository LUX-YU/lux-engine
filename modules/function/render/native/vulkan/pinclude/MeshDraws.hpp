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
        create(DeviceSession&, ExecutionContext&, std::uint32_t, GraphResourceId, const void*) noexcept;
        ~MeshDraws() noexcept;

    private:
        MeshDraws(DeviceSession&, ExecutionContext&, std::uint32_t, GraphResourceId, MeshInstanceDrawGroup) noexcept;
        [[nodiscard]] RenderResult<void> prepare() noexcept;
        void record(VkCommandBuffer) noexcept;
        void update(MeshInstanceId, const MeshInstanceDescription*) noexcept;
        [[nodiscard]] bool selected(const MeshInstanceDescription&) const noexcept;
        DeviceSession& session_;
        ExecutionContext& context_;
        std::uint32_t data_index_;
        GraphResourceId resource_;
        StorageResource sections_;
        StorageSubscription section_subscription_;
        MeshInstanceDrawGroup selection_;
        MeshInstanceStorage::Observer observer_;
        std::unique_ptr<Buffer> buffer_;
        RetirementReservation retirement_;
        std::vector<std::uint32_t> words_, positions_;
        bool available_ = true;
    };
} // namespace lux::render::vulkan
