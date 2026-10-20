#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/graph/CompiledGraph.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageSet.hpp>

namespace lux::render::detail
{
    class OffscreenTarget;
    class UploadContext;

    class LUX_RENDER_PUBLIC ExecutionContext final
    {
    public:
        ~ExecutionContext() noexcept;
        ExecutionContext(const ExecutionContext&) = delete;
        ExecutionContext& operator=(const ExecutionContext&) = delete;

        [[nodiscard]] nri::Device* device() const noexcept
        {
            return code_->device();
        }

        [[nodiscard]] const CompiledGraph& code() const noexcept
        {
            return *code_;
        }

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return target_;
        }

        [[nodiscard]] RenderResult<void> setContextParameters(std::span<const std::byte>) noexcept;

        [[nodiscard]] std::span<const std::byte> contextParameters() const noexcept
        {
            return context_values_;
        }

        [[nodiscard]] RenderResult<void> setParameters(GraphPassId, std::span<const std::byte>) noexcept;
        [[nodiscard]] std::span<const std::byte> parameters(GraphPassId) const noexcept;

        void setViewport(const RasterViewport& viewport) noexcept
        {
            viewport_ = viewport;
        }

        [[nodiscard]] const RasterViewport& viewport() const noexcept
        {
            return viewport_;
        }

        // DeviceSession selects the reusable slot and owns the recording/submission segment.
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t slot, UploadContext*) noexcept;

        [[nodiscard]] const ExecutionProgress& progress() const noexcept
        {
            return progress_;
        }

        [[nodiscard]] StorageSet& attachmentStorages() noexcept
        {
            return storages_;
        }

        template <class Visitor> void forEachStorage(Visitor&& visitor) const noexcept
        {
            for (const auto& use : storage_uses_)
            {
                visitor(code_->resources()[use.resource].key, use.storage_id);
            }
        }

        // Trusted compiled resource index and Session slot, not a business resource lookup.
        void setResource(std::uint32_t resource, std::uint32_t slot, const StorageResource&) noexcept;
        [[nodiscard]] StorageId storageId(std::uint32_t resource) const noexcept;
        [[nodiscard]] nri::Buffer* nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept;

    private:
        friend class DeviceSession;
        friend void recordGraph(ExecutionContext&, nri::CommandBuffer&, std::uint32_t) noexcept;
        friend void recordGraphPreparation(ExecutionContext&, nri::CommandBuffer&, std::uint32_t) noexcept;
        void withdraw() noexcept;
        void submitted(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        [[nodiscard]] RenderResult<void> rebind(std::uint32_t slot) noexcept;

        struct Binding final
        {
            nri::Texture* image = nullptr;
            nri::Descriptor* view = nullptr;
            nri::Descriptor* buffer_view = nullptr;
            nri::Buffer* buffer = nullptr;
            std::uint64_t buffer_offset = 0, buffer_size = 0;
            std::span<const std::byte> parameters;
            const rdesc::ShaderBlock* parameter_layout = nullptr;
            bool dirty = false;
        };

        struct Pass final
        {
            const CompiledPass* recipe;
            std::vector<nri::TextureBarrierDesc> images;
            std::vector<nri::BufferBarrierDesc> buffers;
            std::vector<nri::DescriptorSet*> descriptors;
            nri::DescriptorSet* private_descriptors = nullptr;
            nri::Buffer* copy_scratch = nullptr; // Only for packed image copies whose native row alignment differs.
            std::uint32_t copy_row_pitch = 0, copy_slice_pitch = 0;
        };

        struct ParameterCopy final
        {
            std::uint32_t resource;
            std::uint32_t source_offset, destination_offset, bytes;
        };

        ExecutionContext(DeviceSession&, const CompiledGraph&, OffscreenTarget&) noexcept;
        DeviceSession* session_;
        struct FrameResources;
        std::array<std::unique_ptr<FrameResources>, 3> frames_;
        const CompiledGraph* code_;
        TargetDesc target_;
        RasterViewport viewport_;
        std::vector<std::uint32_t> storage_indices_;

        struct StorageUse final
        {
            ExecutionContext* context;
            StorageId storage_id;
            std::uint32_t resource;
            StorageSubscription subscription;
            void* native = nullptr;
            const StorageBindingOps* ops = nullptr;
        };

        StorageSet storages_;
        std::vector<StorageUse> storage_uses_;
        std::vector<FeatureFrameWork> context_data_;
        std::vector<std::uint32_t> prepared_data_;
        std::vector<std::vector<std::byte>> pass_parameters_;
        std::vector<std::vector<ParameterCopy>> parameter_copies_;
        std::vector<std::byte> context_values_;
        ExecutionProgress progress_;
    };
} // namespace lux::render::detail
