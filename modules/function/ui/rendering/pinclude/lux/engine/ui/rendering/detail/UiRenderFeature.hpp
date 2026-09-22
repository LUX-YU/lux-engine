#pragma once

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/ui/rendering/UiRenderFeature.hpp>
#include <lux/engine/ui/rendering/detail/UiVulkanBackend.hpp>

namespace lux::ui::detail
{
inline constexpr render::TypeId kUiFrameAttachment = 0x75524631;
inline constexpr render::TypeId kUiSceneUseAttachment = 0x75527331;

class UiRenderFeature final : public render::RenderFeature
{
  public:
    explicit UiRenderFeature(UiFontAtlasSnapshot font);
    ~UiRenderFeature() override;
    [[nodiscard]] std::string_view name() const override
    {
        return "UiRender";
    }
    [[nodiscard]] render::Expected<void> initAndAttachTo(render::RenderScene &) override;
    void addPasses(render::RGBuilder &builder) override;
    [[nodiscard]] std::span<const render::SampledTarget> sampledTargets() const noexcept override;
    [[nodiscard]] render::Expected<void> bindSampledTargets(std::span<const render::SampledTargetImage>,
                                                            std::uint32_t frame_slot, std::uint64_t serial) override;
    void clear() noexcept;
    void adopt(std::shared_ptr<const UiRenderFrame> frame);

  private:
    UiFontAtlasSnapshot font_;
    std::unique_ptr<UiVulkanRenderer> renderer_;
    std::vector<std::pair<VkFormat, VkPipeline>> pipelines_;
    std::shared_ptr<const UiRenderFrame> frame_;
    std::vector<render::SampledTarget> reads_;
    struct Texture final
    {
        std::uint64_t token{};
        VkImageView image{};
        VkDescriptorSet descriptor{};
    };
    std::vector<std::vector<Texture>> textures_;
    std::uint32_t frame_slot_{};
    std::uint64_t serial_{};
    VkSampler sampler_{};
    static VkDescriptorSet resolveTexture(void *, TextureHandle) noexcept;
};
} // namespace lux::ui::detail
