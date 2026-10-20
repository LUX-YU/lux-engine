#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/features/FeatureBinding.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageCreateContext.hpp>

namespace lux::render::detail
{
    class OffscreenTarget;
    class FrameExecution;
    class ResourceUploader;

    class LUX_RENDER_PUBLIC RenderGraphInstance final
    {
    public:
        RenderGraphInstance(
            DeviceSession&,
            std::uint32_t frame_count,
            const RenderPlan&,
            OffscreenTarget* = nullptr
        ) noexcept;
        [[nodiscard]] RenderResult<void> bindStorage(
            std::uint32_t resource,
            StorageId,
            const StorageBindingOps&,
            void* native,
            const void* argument
        ) noexcept;
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        void addFeatureBinding(GraphResourceId resource, FeatureBinding binding) noexcept;
        // Borrowed physical inputs are installed before initialize; the caller retains them through GPU completion.
        void bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept;
        [[nodiscard]] RenderResult<void> bindSampler(std::uint32_t input, nri::Descriptor&) noexcept;
        ~RenderGraphInstance() noexcept;
        RenderGraphInstance(const RenderGraphInstance&) = delete;
        RenderGraphInstance& operator=(const RenderGraphInstance&) = delete;

        [[nodiscard]] nri::Device* device() const noexcept
        {
            return plan_->device();
        }

        [[nodiscard]] const RenderPlan& plan() const noexcept
        {
            return *plan_;
        }

        [[nodiscard]] const TargetDesc& target() const noexcept
        {
            return target_;
        }

        [[nodiscard]] RenderResult<void> setInstanceParameters(std::span<const std::byte>) noexcept;

        [[nodiscard]] std::span<const std::byte> instanceParameters() const noexcept
        {
            return instance_values_;
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

        // FrameExecution selects the reusable slot and owns the recording/submission segment.
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t slot, ResourceUploader*) noexcept;

        [[nodiscard]] const ExecutionProgress& progress() const noexcept
        {
            return progress_;
        }

        template <class Visitor> void forEachStorage(Visitor&& visitor) const noexcept
        {
            for (const auto& use : storage_uses_)
            {
                visitor(plan_->resources()[use.resource].key, use.storage_id);
            }
        }

        // Trusted compiled resource index and Session slot, not a business resource lookup.
        void setResource(std::uint32_t resource, std::uint32_t slot, const StorageResource&) noexcept;
        [[nodiscard]] StorageId storageId(std::uint32_t resource) const noexcept;
        // Cold diagnostics: sum actual NRI allocation requirements for this instance's owned resources.
        [[nodiscard]] bool dumpResources(GraphDumpSink) const noexcept;
        [[nodiscard]] nri::Buffer* nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept;

    private:
        friend class FrameExecution;
        friend class RenderExecutor;
        void disconnectStorageBindings() noexcept;
        void submitted(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> createFrameResources(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareFrameBindings(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        [[nodiscard]] RenderResult<void> rebind(std::uint32_t slot) noexcept;

        struct Binding final
        {
            nri::Texture* image = nullptr;
            nri::Descriptor* view = nullptr;
            nri::Buffer* buffer = nullptr;
            std::uint64_t buffer_offset = 0, buffer_size = 0;
            std::span<const std::byte> parameters;
            const rdesc::ShaderBlock* parameter_layout = nullptr;
            bool dirty = false;
        };

        struct Pass final
        {
            const CompiledPass* compiled_pass;
            std::vector<nri::TextureBarrierDesc> images;
            std::vector<nri::BufferBarrierDesc> buffers;
            std::vector<nri::DescriptorSet*> descriptors;
            nri::DescriptorSet* private_descriptors = nullptr;
            std::vector<nri::Descriptor*> private_views;
            std::vector<nri::AttachmentDesc> colors;
            nri::AttachmentDesc depth{};
            std::vector<nri::TextureDataLayoutDesc> copy_layouts;
            std::vector<nri::Descriptor*> clear_views;
        };

        struct ParameterCopy final
        {
            std::uint32_t resource;
            std::uint32_t source_offset, destination_offset, bytes;
        };

        DeviceSession* session_;
        struct FrameResources;
        std::vector<FrameResources> frames_;
        const RenderPlan* plan_;
        TargetDesc target_;
        RasterViewport viewport_;
        std::vector<std::uint32_t> storage_indices_;

        struct StorageUse final
        {
            RenderGraphInstance* instance;
            StorageId storage_id;
            std::uint32_t resource;
            StorageSubscription subscription;
            void* native = nullptr;
            const StorageBindingOps* ops = nullptr;
        };

        OffscreenTarget* output_;
        std::vector<nri::Descriptor*> samplers_;
        std::vector<GraphResourceId> feature_resources_;
        std::vector<StorageUse> storage_uses_;
        std::vector<FeatureBinding> feature_bindings_;
        std::vector<std::span<const FeatureBinding::BufferCopy>> prepared_copies_;
        std::vector<std::vector<std::byte>> pass_parameters_;
        std::vector<std::vector<ParameterCopy>> parameter_copies_;
        std::vector<std::byte> instance_values_;
        ExecutionProgress progress_;
    };
} // namespace lux::render::detail
