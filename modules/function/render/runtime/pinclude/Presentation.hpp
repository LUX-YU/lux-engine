#pragma once
#include <lux/engine/render/vulkan/graph/Executable.hpp>

namespace lux::render::detail
{
    struct PresentationLayer
    {
        vulkan::NativeGraphInvocations* invocations;
        vulkan::GraphSubmission receipt;
        GraphResourceId output;
        VkRect2D rectangle;
        bool copied{};
        const vulkan::Image* source{};
        vulkan::NativeRangeState source_state;
    };

    // Owns one real WSI target. Window is an explicit host borrow; Surface,
    // Swapchain, acquire/present synchronization and readback storage are unique.
    class PresentationTarget
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<PresentationTarget>> create(
            const vulkan::VulkanDevice& device,
            const vulkan::VulkanAllocator& allocator,
            vulkan::SubmissionQueue& queue,
            void* window_instance,
            void* window,
            VkExtent2D extent,
            std::uint32_t frames
        ) noexcept;
        ~PresentationTarget() noexcept;
        PresentationTarget(const PresentationTarget&) = delete;
        PresentationTarget& operator=(const PresentationTarget&) = delete;
        [[nodiscard]] RenderResult<void> resize(VkExtent2D extent) noexcept;
        [[nodiscard]] RenderResult<bool> present(
            std::span<PresentationLayer> layers,
            std::uint32_t frame_slot
        ) noexcept;
        [[nodiscard]] RenderResult<void> validateSource(VkFormat format) const noexcept;
        [[nodiscard]] RenderResult<void> readback(std::uint64_t offset, std::span<std::byte> bytes) noexcept;
        [[nodiscard]] VkExtent2D extent() const noexcept;
        [[nodiscard]] bool available() const noexcept;
        [[nodiscard]] std::uint64_t presents() const noexcept;

    private:
        struct Backing;
        explicit PresentationTarget(std::unique_ptr<Backing> backing) noexcept;
        std::unique_ptr<Backing> backing_;
    };
} // namespace lux::render::detail
