#pragma once

#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageSet.hpp>

namespace lux::render::vulkan
{
    class SubmissionQueue;
    class CommandBatch;

    class ExecutionContext final
    {
    public:
        ~ExecutionContext() noexcept;
        ExecutionContext(const ExecutionContext&) = delete;
        ExecutionContext& operator=(const ExecutionContext&) = delete;

        [[nodiscard]] VkDevice device() const noexcept
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

        [[nodiscard]] RenderResult<void> setContextParameters(std::span<const std::byte> bytes) noexcept;

        [[nodiscard]] std::span<const std::byte> contextParameters() const noexcept
        {
            return context_values_;
        }

        [[nodiscard]] RenderResult<void> setParameters(GraphPassId pass, std::span<const std::byte> bytes) noexcept;
        [[nodiscard]] std::span<const std::byte> parameters(GraphPassId pass) const noexcept;

        // Owner-thread caller has validated the viewport at its request boundary.
        void setViewport(const RasterViewport& viewport) noexcept
        {
            viewport_ = viewport;
        }

        [[nodiscard]] const RasterViewport& viewport() const noexcept
        {
            return viewport_;
        }

        // Session prepares all participating Contexts before opening the shared recording.
        [[nodiscard]] RenderResult<void> prepareFrame() noexcept;
        void record(CommandBatch& batch) noexcept;

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

        // Native Feature contributions use already compiled resource indices. No name lookup or
        // professional Storage behavior is performed by the Context.
        void setResource(std::uint32_t resource, const StorageResource&) noexcept;
        void invalidateData(std::uint32_t index) noexcept;
        [[nodiscard]] StorageId storageId(std::uint32_t resource) const noexcept;

        // Compiled resource indices are trusted here; this is a Native diagnostic borrow.
        [[nodiscard]] VkBuffer nativeBuffer(std::uint32_t resource) const noexcept
        {
            return bindings_[resource].buffer;
        }

    private:
        friend class DeviceSession;
        void withdraw() noexcept;
        void submitted() noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        void rebind() noexcept;

        struct Binding final
        {
            gapi::vk::VmaImage* image = nullptr;
            VkImageView view{};
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize buffer_offset = 0, buffer_size = 0;
            std::span<const std::byte> parameters;
            const rdesc::ShaderBlock* parameter_layout = nullptr;
            std::uint32_t storage_use = kGraphInvalidIndex;
            bool dirty = false;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> barriers, descriptors, parameter_copies;
        };

        struct Pass final
        {
            const CompiledPass* recipe;
            std::vector<VkImageMemoryBarrier2> images;
            std::vector<VkBufferMemoryBarrier2> buffers;
            std::vector<VkDescriptorSet> descriptors;
            VkDescriptorSet private_descriptors = VK_NULL_HANDLE;
            std::vector<std::byte> parameters;

            struct ParameterCopy final
            {
                std::uint32_t resource; // kGraphInvalidIndex selects this Context's value array.
                std::uint32_t source_offset, destination_offset, bytes;
            };

            std::vector<ParameterCopy> parameter_copies;
        };

        ExecutionContext(
            DeviceSession& owner,
            SubmissionQueue& queue,
            const CompiledGraph& code,
            TargetDesc target
        ) noexcept;
        DeviceSession* owner_;
        SubmissionQueue* queue_;
        struct FrameResources;
        std::unique_ptr<FrameResources> frame_resources_;
        std::size_t unavailable_buffers_ = 0;
        const CompiledGraph* code_;
        TargetDesc target_;
        RasterViewport viewport_;
        std::vector<Binding> bindings_;

        struct StorageUse final
        {
            ExecutionContext* context;
            StorageId storage_id;
            std::uint32_t resource;
            StorageSubscription subscription;
        };

        StorageSet storages_;
        std::vector<StorageUse> storage_uses_;
        std::vector<std::uint32_t> changed_resources_;
        std::vector<ContextData> context_data_;
        std::vector<std::uint32_t> changed_data_;

        std::vector<Pass> passes_;
        std::vector<std::byte> context_values_;

        ExecutionProgress progress_;
    };
} // namespace lux::render::vulkan
