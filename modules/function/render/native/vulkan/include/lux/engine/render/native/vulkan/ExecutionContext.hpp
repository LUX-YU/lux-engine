#pragma once

#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/resource/StorageSet.hpp>

namespace lux::render::vulkan
{

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

        [[nodiscard]] RenderResult<EExecutionOutcome> execute() noexcept;

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

        // Compiled resource indices are trusted here; this is a Native diagnostic borrow.
        [[nodiscard]] VkBuffer nativeBuffer(std::uint32_t resource) const noexcept
        {
            return bindings_[resource].buffer ? bindings_[resource].buffer->native() : VK_NULL_HANDLE;
        }

    private:
        friend class DeviceSession;
        void withdraw() noexcept;
        [[nodiscard]] RenderResult<void> prepareDescriptors() noexcept;
        void rebind() noexcept;

        struct Binding final
        {
            Image* image = nullptr;
            VkImageView view{};
            Buffer* buffer = nullptr;
            VkDeviceSize buffer_offset = 0;
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
                const std::span<const std::byte>* source;
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
        RetirementReservation retirement_;
        bool bindings_dirty_ = false;
        const CompiledGraph* code_;
        TargetDesc target_;
        RasterViewport viewport_;
        std::vector<Binding> bindings_;

        struct StorageUse final
        {
            StorageId storage_id;
            PreparedStorageBinding binding;
            std::uint32_t resource;
        };

        StorageSet storages_;
        std::vector<StorageUse> storage_uses_;
        std::vector<std::uint32_t> pending_updates_;

        std::vector<Pass> passes_;
        std::vector<std::byte> context_values_;
        std::span<const std::byte> context_view_; // Stable range after prepare, used by compiled parameter copies.

        ExecutionProgress progress_;
    };
} // namespace lux::render::vulkan
