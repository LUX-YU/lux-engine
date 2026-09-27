#pragma once

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/ui/rendering/detail/VulkanBackend.hpp>

namespace lux::ui::detail
{
    inline constexpr render::TypeId kFrameAttachment = 0x75524631;
    struct FrameInput final
    {
        render::RenderSubmissionState submission;
        std::shared_ptr<const RenderFrame> frame; // Released before the submission fact.
    };

    class RenderFeature final : public render::RenderFeature
    {
    public:
        explicit RenderFeature(FontAtlas font);
        ~RenderFeature() override;
        [[nodiscard]] std::string_view name() const override
        {
            return "UiRender";
        }
        [[nodiscard]] render::Expected<void> initAndAttachTo(render::RenderScene&) override;
        void addPasses(render::RGBuilder& builder) override;
        void retainSubmissions(const render::FrameRuntime&) const noexcept override;
        [[nodiscard]] std::span<const render::SampledTarget> sampledTargets() const noexcept override;
        [[nodiscard]] render::Expected<void> bindSampledTargets(
            std::span<const render::SampledTargetImage>,
            std::uint32_t frame_slot,
            std::uint64_t serial
        ) override;
        void clear() noexcept;
        [[nodiscard]] render::Expected<void> adopt(const FrameInput& input);

    private:
        FontAtlas font_;
        std::unique_ptr<VulkanRenderer> renderer_;
        std::vector<std::pair<VkFormat, VkPipeline>> pipelines_;
        std::shared_ptr<const RenderFrame> frame_;
        render::RenderSubmissionState submission_;
        std::vector<render::SampledTarget> reads_;
        struct Texture final
        {
            render::RTextureHandle token;
            VkImageView image{};
            VkSampler sampler{};
            VkDescriptorSet descriptor{};
        };
        std::vector<std::vector<Texture>> textures_;
        std::uint32_t frame_slot_{};
        std::uint64_t serial_{};
        VkSampler sampler_{};
        static VkDescriptorSet resolveTexture(void*, render::RTextureHandle) noexcept;
    };
} // namespace lux::ui::detail
