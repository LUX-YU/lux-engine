#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <lux/engine/ui/detail/ImGuiDrawDataSnapshot.hpp>
#include <vulkan/vulkan.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
void check(VkResult value)
{
    assert(value == VK_SUCCESS);
}

std::uint32_t memoryType(VkPhysicalDevice gpu, std::uint32_t mask, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    {
        if ((mask & (1U << index)) && (properties.memoryTypes[index].propertyFlags & flags) == flags)
        {
            return index;
        }
    }
    assert(false && "Required Vulkan memory type is unavailable");
    return 0;
}

void drawAfterContextDestroyed(lux::ui::detail::ImGuiDrawDataSnapshot &snapshot, std::vector<unsigned char> &atlas,
                               int atlas_width, int atlas_height)
{
    assert(ImGui::GetCurrentContext() == nullptr);
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "LUX UI Context lifetime regression";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    VkInstance instance{};
    check(vkCreateInstance(&instance_info, nullptr, &instance));
    std::uint32_t gpu_count{};
    check(vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr));
    assert(gpu_count != 0);
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    check(vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data()));
    const auto gpu = gpus.front();
    std::uint32_t queue_count{};
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queue_count, queues.data());
    std::uint32_t family{};
    while (family < queue_count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    {
        ++family;
    }
    assert(family < queue_count);
    const float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    dynamic.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &dynamic;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device{};
    check(vkCreateDevice(gpu, &device_info, nullptr, &device));
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = instance;
    init.PhysicalDevice = gpu;
    init.Device = device;
    init.QueueFamily = family;
    init.Queue = queue;
    init.DescriptorPoolSize = 64;
    init.MinImageCount = 2;
    init.ImageCount = 2;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo = rendering;
    init.CheckVkResultFn = check;
    auto *renderer = ImGui_ImplVulkan_CreateRendererEx(&init);
    assert(renderer);
    ImGui_ImplVulkan_SetTextureResolverEx(
        renderer,
        [](ImGui_ImplVulkan_Renderer *owner, ImTextureID token, void *) -> VkDescriptorSet {
            assert(token == 0);
            return ImGui_ImplVulkan_GetFontsTextureDescriptorSetEx(owner);
        },
        nullptr);
    assert(ImGui_ImplVulkan_CreateFontsTextureEx(renderer, atlas.data(), atlas_width, atlas_height));
    assert(ImGui::GetCurrentContext() == nullptr);

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {256, 128, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage image{};
    check(vkCreateImage(device, &image_info, nullptr, &image));
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(gpu, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory image_memory{};
    check(vkAllocateMemory(device, &allocation, nullptr, &image_memory));
    check(vkBindImageMemory(device, image, image_memory, 0));
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{};
    check(vkCreateImageView(device, &view_info, nullptr, &view));

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = 256 * 128 * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{};
    check(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType(gpu, requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory buffer_memory{};
    check(vkAllocateMemory(device, &allocation, nullptr, &buffer_memory));
    check(vkBindBufferMemory(device, buffer, buffer_memory, 0));
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool{};
    check(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command{};
    check(vkAllocateCommandBuffers(device, &command_info, &command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &begin));
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = view_info.subresourceRange;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo pass{VK_STRUCTURE_TYPE_RENDERING_INFO};
    pass.renderArea.extent = {256, 128};
    pass.layerCount = 1;
    pass.colorAttachmentCount = 1;
    pass.pColorAttachments = &attachment;
    vkCmdBeginRendering(command, &pass);
    ImGui_ImplVulkan_RenderDrawDataEx(renderer, const_cast<ImDrawData *>(&snapshot.drawData()), command);
    vkCmdEndRendering(command);
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {256, 128, 1};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                         0, nullptr);
    check(vkEndCommandBuffer(command));
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    check(vkCreateFence(device, &fence_info, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(queue, 1, &submit, fence));
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ULL));
    void *mapped{};
    check(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    const auto *pixel = static_cast<const std::uint8_t *>(mapped) + (40 * 256 + 40) * 4;
    assert(pixel[0] >= 250 && pixel[1] >= 60 && pixel[1] <= 68 && pixel[2] >= 28 && pixel[2] <= 36);
    const auto *background = static_cast<const std::uint8_t *>(mapped) + (120 * 256 + 200) * 4;
    assert(background[0] == 0 && background[1] == 0 && background[2] == 0);
    std::cout << "PASS context-free Ex create/font/draw/destroy; pixel=" << int(pixel[0]) << ',' << int(pixel[1]) << ','
              << int(pixel[2]) << "; completed=fence\n";
    vkUnmapMemory(device, buffer_memory);
    ImGui_ImplVulkan_DestroyRendererEx(renderer);
    assert(ImGui::GetCurrentContext() == nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, buffer_memory, nullptr);
    vkDestroyImageView(device, view, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, image_memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}
} // namespace

int main()
{
    auto *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {256, 128};
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::vector<unsigned char> atlas(pixels, pixels + width * height * 4);
    io.Fonts->SetTexID(0);
    ImGui::NewFrame();
    ImGui::GetBackgroundDrawList()->AddRectFilled({8, 8}, {120, 100}, IM_COL32(255, 64, 32, 255));
    ImGui::Render();
    lux::ui::detail::ImGuiDrawDataSnapshot snapshot;
    assert(snapshot.capture(*ImGui::GetDrawData()));
    ImGui::DestroyContext(context);
    std::thread render([&] { drawAfterContextDestroyed(snapshot, atlas, width, height); });
    render.join();
}
