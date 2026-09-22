#include <lux/engine/ui/rendering/detail/UiVulkanBackend.hpp>

#include <imgui_impl_vulkan.h>

#include <new>
#include <utility>

namespace lux::ui::detail
{
struct UiVulkanRenderer::Impl final
{
    ImGui_ImplVulkan_Renderer *renderer{};
    void *render_buffers{};
    TextureResolver resolve{};
    void *resolver_user{};
    std::uint64_t uploaded_serial{};
    bool has_uploaded{};

    static VkDescriptorSet resolveTexture(ImGui_ImplVulkan_Renderer *renderer, ImTextureID texture, void *user)
    {
        if (texture == ImTextureID{})
        {
            return ImGui_ImplVulkan_GetFontsTextureDescriptorSetEx(renderer);
        }
        auto &self = *static_cast<Impl *>(user);
        return self.resolve ? self.resolve(self.resolver_user, TextureHandle{texture}) : VK_NULL_HANDLE;
    }
};

UiVulkanRenderer::UiVulkanRenderer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

UiVulkanRenderer::CreateResult UiVulkanRenderer::create(const UiVulkanRendererCreateInfo &info,
                                                        const UiFontAtlasSnapshot &font) noexcept
{
    const bool invalid_handles = info.instance == VK_NULL_HANDLE || info.physical_device == VK_NULL_HANDLE ||
                                 info.device == VK_NULL_HANDLE || info.queue == VK_NULL_HANDLE;
    if (invalid_handles || info.color_format == VK_FORMAT_UNDEFINED || info.image_count == 0U || font.pixels.empty() ||
        font.width <= 0 || font.height <= 0)
    {
        return lux::cxx::unexpected(EUiVulkanBackendError::INVALID_CONFIG);
    }
    try
    {
        auto impl = std::make_unique<Impl>();
        VkPipelineRenderingCreateInfoKHR pipeline{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
        pipeline.colorAttachmentCount = 1U;
        pipeline.pColorAttachmentFormats = &info.color_format;
        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = info.instance;
        init.PhysicalDevice = info.physical_device;
        init.Device = info.device;
        init.QueueFamily = info.queue_family;
        init.Queue = info.queue;
        init.DescriptorPoolSize = 64U;
        init.MinImageCount = info.image_count;
        init.ImageCount = info.image_count;
        init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init.Allocator = info.allocator;
        init.UseDynamicRendering = true;
        init.PipelineRenderingCreateInfo = pipeline;
        impl->renderer = ImGui_ImplVulkan_CreateRendererEx(&init);
        if (impl->renderer == nullptr)
        {
            return lux::cxx::unexpected(EUiVulkanBackendError::RENDERER_CREATE_FAILURE);
        }
        ImGui_ImplVulkan_SetTextureResolverEx(impl->renderer, &Impl::resolveTexture, impl.get());
        if (!ImGui_ImplVulkan_CreateFontsTextureEx(impl->renderer, const_cast<unsigned char *>(font.pixels.data()),
                                                   font.width, font.height))
        {
            ImGui_ImplVulkan_DestroyRendererEx(impl->renderer);
            return lux::cxx::unexpected(EUiVulkanBackendError::FONT_UPLOAD_FAILURE);
        }
        impl->render_buffers = ImGui_ImplVulkan_CreateRenderBuffersEx(impl->renderer);
        if (impl->render_buffers == nullptr)
        {
            ImGui_ImplVulkan_DestroyFontsTextureEx(impl->renderer);
            ImGui_ImplVulkan_DestroyRendererEx(impl->renderer);
            return lux::cxx::unexpected(EUiVulkanBackendError::BUFFER_CREATE_FAILURE);
        }
        return std::unique_ptr<UiVulkanRenderer>{new UiVulkanRenderer(std::move(impl))};
    }
    catch (const std::bad_alloc &)
    {
        return lux::cxx::unexpected(EUiVulkanBackendError::RENDERER_CREATE_FAILURE);
    }
}

UiVulkanRenderer::~UiVulkanRenderer()
{
    if (impl_ == nullptr || impl_->renderer == nullptr)
    {
        return;
    }
    if (impl_->render_buffers != nullptr)
    {
        ImGui_ImplVulkan_DestroyRenderBuffersEx(impl_->renderer, impl_->render_buffers);
    }
    ImGui_ImplVulkan_DestroyFontsTextureEx(impl_->renderer);
    ImGui_ImplVulkan_DestroyRendererEx(impl_->renderer);
}

void UiVulkanRenderer::setTextureResolver(TextureResolver resolver, void *user) noexcept
{
    impl_->resolve = resolver;
    impl_->resolver_user = user;
}

VkDescriptorSet UiVulkanRenderer::addTexture(VkSampler sampler, VkImageView view, VkImageLayout layout)
{
    return ImGui_ImplVulkan_AddTextureEx(impl_->renderer, sampler, view, layout);
}

void UiVulkanRenderer::removeTexture(VkDescriptorSet descriptor) noexcept
{
    if (descriptor == VK_NULL_HANDLE)
    {
        return;
    }
    ImGui_ImplVulkan_RemoveTextureEx(impl_->renderer, descriptor);
}

void UiVulkanRenderer::render(const UiFrameSnapshot *snapshot, VkCommandBuffer command) noexcept
{
    render(snapshot, command, VK_NULL_HANDLE);
}

VkPipeline UiVulkanRenderer::createColorPipeline(VkFormat format)
{
    return ImGui_ImplVulkan_CreateColorPipelineEx(impl_->renderer, format);
}

void UiVulkanRenderer::destroyColorPipeline(VkPipeline pipeline) noexcept
{
    ImGui_ImplVulkan_DestroyColorPipelineEx(impl_->renderer, pipeline);
}

void UiVulkanRenderer::renderFrame(const UiFrameSnapshot &snapshot, VkCommandBuffer command, VkPipeline pipeline,
                                   std::uint32_t frame_slot, std::uint64_t serial) noexcept
{
    auto *data = const_cast<ImDrawData *>(static_cast<const ImDrawData *>(snapshot.nativeDrawData()));
    if (!data || !data->Valid || data->TotalIdxCount <= 0)
    {
        return;
    }
    const bool upload = !impl_->has_uploaded || impl_->uploaded_serial != serial;
    ImGui_ImplVulkan_RenderDrawDataAtFrameEx(impl_->renderer, data, command, impl_->render_buffers, frame_slot, upload,
                                             pipeline);
    impl_->uploaded_serial = serial;
    impl_->has_uploaded = true;
}

void UiVulkanRenderer::render(const UiFrameSnapshot *snapshot, VkCommandBuffer command, VkPipeline pipeline) noexcept
{
    if (impl_ == nullptr || impl_->renderer == nullptr || snapshot == nullptr || command == VK_NULL_HANDLE)
    {
        return;
    }
    auto *draw_data = const_cast<ImDrawData *>(static_cast<const ImDrawData *>(snapshot->nativeDrawData()));
    if (draw_data == nullptr)
    {
        return;
    }
    if (!draw_data->Valid || draw_data->TotalIdxCount <= 0)
    {
        return;
    }
    ImGui_ImplVulkan_RenderDrawDataWithBuffersEx(impl_->renderer, draw_data, command, impl_->render_buffers, pipeline);
}
} // namespace lux::ui::detail
