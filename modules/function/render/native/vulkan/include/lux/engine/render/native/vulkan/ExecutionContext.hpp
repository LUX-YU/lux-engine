#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>

namespace lux::render::vulkan
{
    // Borrowed validated identities, not another identity allocator or owner.
    struct ScopeIdentity final
    {
        SceneId scene;
        ViewId view;
    };

    class ExecutionContext final
    {
    public:
        ~ExecutionContext() noexcept;
        ExecutionContext(const ExecutionContext&) = delete;
        ExecutionContext& operator=(const ExecutionContext&) = delete;
        [[nodiscard]] VkDevice device() const noexcept { return code_->device(); }
        [[nodiscard]] const CompiledGraph& code() const noexcept { return *code_; }
        [[nodiscard]] const TargetDesc& target() const noexcept { return target_; }
        [[nodiscard]] std::optional<SubmissionTicket> lastUse() const noexcept { return last_use_; }
        void setColor(std::array<float, 4> color) noexcept { color_ = color; } // validated cold View state
        [[nodiscard]] std::array<float, 4> color() const noexcept { return color_; }
        [[nodiscard]] RenderResult<bool> execute() noexcept;
        [[nodiscard]] RenderResult<bool> completed() noexcept;
        [[nodiscard]] RenderResult<std::vector<std::byte>> readback(std::uint32_t resource = kGraphInvalidIndex) noexcept;
        [[nodiscard]] std::size_t readbackBytes() const noexcept { return bindings_[readback_].buffer->size(); }
        // Compiled resource indices are trusted here; this is a Native diagnostic borrow.
        [[nodiscard]] VkBuffer nativeBuffer(std::uint32_t resource) const noexcept
        {
            return bindings_[resource].buffer ? bindings_[resource].buffer->native() : VK_NULL_HANDLE;
        }
        // Server pauses re-execution while a deferred readback consumes this result.
        void retainReadback() noexcept { ++readers_; }
        void releaseReadback() noexcept { --readers_; }

    private:
        friend struct NativeMemoryAudit;
        friend class DeviceSession;
        struct Binding final
        {
            BufferAllocation* owned_buffer = nullptr;
            ImageAllocation* image = nullptr;
            VkImageView view{};
            BufferAllocation* buffer = nullptr;
        };
        struct Pass final
        {
            const CompiledPass* recipe;
            std::vector<VkImageMemoryBarrier2> images;
            std::vector<VkBufferMemoryBarrier2> buffers;
        };
        ExecutionContext(DeviceSession& owner, SubmissionQueue& queue, std::shared_ptr<const CompiledGraph> code, TargetDesc target) noexcept;
        DeviceSession* owner_;
        SubmissionQueue* queue_;
        std::shared_ptr<const CompiledGraph> code_;
        TargetDesc target_;
        std::vector<Binding> bindings_;
        std::vector<ScopedBuffer*> leases_;
        std::vector<Pass> passes_;
        std::vector<VkBufferMemoryBarrier2> host_barriers_;
        std::optional<SubmissionTicket> last_use_;
        std::uint32_t readback_ = kGraphInvalidIndex;
        std::uint32_t readers_ = 0;
        std::array<float, 4> color_{1, 0, 0, 1};
    };
}
