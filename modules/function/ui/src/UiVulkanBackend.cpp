#include <lux/engine/ui/detail/UiVulkanBackend.hpp>
#include <lux/engine/ui/detail/ImGuiContextLease.hpp>

#include <imgui_impl_vulkan.h>

#include <new>
#include <utility>

namespace lux::ui::detail
{
    struct UiVulkanRenderer::Impl final
    {
        ImGui_ImplVulkan_Renderer* renderer{};
        void* render_buffers{};
        void* context{};
    };

    namespace
    {
        VkDescriptorSet resolveTexture(ImGui_ImplVulkan_Renderer* renderer, ImTextureID texture, void*)
        {
            return texture == ImTextureID{} ? ImGui_ImplVulkan_GetFontsTextureDescriptorSetEx(renderer) :
                                              VK_NULL_HANDLE;
        }
    }

    UiVulkanRenderer::UiVulkanRenderer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    UiVulkanRenderer::CreateResult UiVulkanRenderer::create(
        const UiVulkanRendererCreateInfo& info,
        const UiFontAtlasSnapshot& font
    ) noexcept
    {
        const bool invalid_handles = info.instance == VK_NULL_HANDLE || info.physical_device == VK_NULL_HANDLE ||
            info.device == VK_NULL_HANDLE || info.queue == VK_NULL_HANDLE;
        if (invalid_handles || info.color_format == VK_FORMAT_UNDEFINED || info.image_count == 0U ||
            font.pixels.empty() || font.width <= 0 || font.height <= 0 || font.context == nullptr)
        {
            return lux::cxx::unexpected(EUiVulkanBackendError::INVALID_CONFIG);
        }
        try
        {
            ImGuiContextLease context{font.context};
            auto impl = std::make_unique<Impl>();
            impl->context = font.context;
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
                return lux::cxx::unexpected(EUiVulkanBackendError::RENDERER_CREATE_FAILURE);
            ImGui_ImplVulkan_SetTextureResolverEx(impl->renderer, &resolveTexture, nullptr);
            if (!ImGui_ImplVulkan_CreateFontsTextureEx(
                    impl->renderer,
                    const_cast<unsigned char*>(font.pixels.data()),
                    font.width,
                    font.height
                ))
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
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EUiVulkanBackendError::RENDERER_CREATE_FAILURE);
        }
    }

    UiVulkanRenderer::~UiVulkanRenderer()
    {
        if (impl_ == nullptr || impl_->renderer == nullptr)
            return;
        ImGuiContextLease context{impl_->context};
        if (impl_->render_buffers != nullptr)
            ImGui_ImplVulkan_DestroyRenderBuffersEx(impl_->renderer, impl_->render_buffers);
        ImGui_ImplVulkan_DestroyFontsTextureEx(impl_->renderer);
        ImGui_ImplVulkan_DestroyRendererEx(impl_->renderer);
    }

    void UiVulkanRenderer::render(const UiDrawDataSnapshot* snapshot, VkCommandBuffer command) noexcept
    {
        if (impl_ == nullptr || impl_->renderer == nullptr || snapshot == nullptr ||
            command == VK_NULL_HANDLE)
        {
            return;
        }
        auto* draw_data = const_cast<ImDrawData*>(static_cast<const ImDrawData*>(snapshot->nativeDrawData()));
        if (draw_data == nullptr)
            return;
        if (!draw_data->Valid || draw_data->TotalIdxCount <= 0)
            return;
        ImGui_ImplVulkan_RenderDrawDataWithBuffersEx(
            impl_->renderer,
            draw_data,
            command,
            impl_->render_buffers
        );
    }
} // namespace lux::ui::detail
