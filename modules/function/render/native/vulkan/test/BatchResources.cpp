#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <array>
#include <fstream>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace foundation_test;

namespace
{
    bool hold_observation = false;
    bool fail_submit = false;
    unsigned binding_destructions = 0;
    unsigned buffer_destructions = 0;
    unsigned image_destructions = 0;
    unsigned array_destructions = 0;

    // The fixture owns real Vulkan resources, not lifetime counters standing in for GPU work.
    struct Bindings final
    {
        VkDevice device;
        std::shared_ptr<DescriptorArray> array;
        DescriptorRange range;
        std::shared_ptr<Image> image;
        std::shared_ptr<Buffer> input, output, indirect;
        VkImageView view{};
        VkSampler sampler{};
        VkPipelineLayout layout{};
        VkPipeline pipeline{};

        explicit Bindings(VkDevice device) noexcept : device(device) {}

        ~Bindings() noexcept
        {
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            if (view) vkDestroyImageView(device, view, nullptr);
            ++binding_destructions;
        }
    };

    struct Parameters final
    {
        VkDeviceAddress input, output;
        std::uint32_t image_index;
    };

    std::vector<std::uint32_t> readSpirv(const char* path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        CHECK(file);
        const auto size = file.tellg();
        CHECK(size > 0 && size % 4 == 0);
        std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
        file.seekg(0);
        CHECK(file.read(reinterpret_cast<char*>(words.data()), size));
        return words;
    }

    VkDeviceAddress address(VkDevice device, const Buffer& buffer)
    {
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = buffer.native();
        const auto result = vkGetBufferDeviceAddress(device, &info);
        CHECK(result != 0);
        return result;
    }

    void imageBarrier(VkCommandBuffer command, VkImage image, bool initialized)
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.image = image;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = initialized ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcStageMask = initialized ? VK_PIPELINE_STAGE_2_CLEAR_BIT : VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = initialized ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_NONE;
        barrier.dstStageMask = initialized ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.dstAccessMask = initialized ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    void run(DeviceSession& session, std::span<const std::uint32_t> spirv)
    {
        const auto previous_bindings = binding_destructions;
        const auto previous_buffers = buffer_destructions;
        const auto previous_images = image_destructions;
        const auto previous_arrays = array_destructions;
        const auto device = session.device().native();
        auto& allocator = session.allocator();
        auto& queue = session.queue(EQueueRole::COMPUTE);
        auto bindings = std::make_shared<Bindings>(device);
        bindings->array = take(session.sampledImageArray());
        auto& array = *bindings->array;
        bindings->range = take(array.allocate());
        const auto retained_index = bindings->range.index();
        CHECK(array.occupied() == 1);
        {
            auto first = take(array.allocate());
            auto second = take(array.allocate());
            const auto first_index = first.index();
            second = std::move(first); // Returns the old slot exactly once.
            CHECK(array.occupied() == 2);
            auto moved = std::move(second);
            CHECK(moved.index() == first_index);
        }
        CHECK(array.occupied() == 1);
        bindings->image = take(allocator.createImage({{1, 1}, VK_FORMAT_R8G8B8A8_UINT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT}));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = bindings->image->native();
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UINT;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(vkCreateImageView(device, &view, nullptr, &bindings->view) == VK_SUCCESS);
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        CHECK(vkCreateSampler(device, &sampler, nullptr, &bindings->sampler) == VK_SUCCESS);
        bindings->range.write(bindings->view, bindings->sampler);

        std::array<std::uint32_t, 8> input{101, 207, 309, 411, 513, 617, 719, 823};
        bindings->input = take(allocator.createBuffer(sizeof(input), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            EMemoryAccess::UPLOAD));
        CHECK(bindings->input->write(0, std::as_bytes(std::span(input))));
        bindings->output = take(allocator.createBuffer(sizeof(input) * 4, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            EMemoryAccess::READBACK));
        bindings->indirect = take(allocator.createBuffer(sizeof(VkDispatchIndirectCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, EMemoryAccess::UPLOAD));
        const VkDispatchIndirectCommand dispatch{static_cast<std::uint32_t>(input.size()), 1, 1};
        CHECK(bindings->indirect->write(0, std::as_bytes(std::span(&dispatch, 1))));
        const Parameters parameters{address(device, *bindings->input), address(device, *bindings->output), retained_index};

        const auto descriptor_layout = array.layout();
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &descriptor_layout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        CHECK(vkCreatePipelineLayout(device, &layout, nullptr, &bindings->layout) == VK_SUCCESS);
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = spirv.size_bytes();
        shader_info.pCode = spirv.data();
        VkShaderModule shader{};
        CHECK(vkCreateShaderModule(device, &shader_info, nullptr, &shader) == VK_SUCCESS);
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.layout = bindings->layout;
        pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.module = shader;
        pipeline.stage.pName = "main";
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &bindings->pipeline) == VK_SUCCESS);
        vkDestroyShaderModule(device, shader, nullptr);

        auto batch = take(queue.begin());
        batch.retain(bindings); // Includes the actual bindless target, BDA pointees, indirect buffer and Pipeline/Layout.
        imageBarrier(batch.native(), bindings->image->native(), false);
        VkClearColorValue color{};
        color.uint32[0] = 3;
        color.uint32[1] = 7;
        color.uint32[2] = 11;
        color.uint32[3] = 19;
        const VkImageSubresourceRange region{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(batch.native(), bindings->image->native(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &color, 1, &region);
        imageBarrier(batch.native(), bindings->image->native(), true);
        vkCmdBindPipeline(batch.native(), VK_PIPELINE_BIND_POINT_COMPUTE, bindings->pipeline);
        const auto descriptor_set = array.descriptors();
        vkCmdBindDescriptorSets(batch.native(), VK_PIPELINE_BIND_POINT_COMPUTE, bindings->layout, 0, 1,
            &descriptor_set, 0, nullptr);
        vkCmdPushConstants(batch.native(), bindings->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
        vkCmdDispatchIndirect(batch.native(), bindings->indirect->native(), 0);
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(batch.native(), &dependency);

        auto output = bindings->output;
        std::weak_ptr<Bindings> weak_bindings = bindings;
        std::weak_ptr<Buffer> weak_input = bindings->input, weak_indirect = bindings->indirect;
        std::weak_ptr<Image> weak_image = bindings->image;
        std::weak_ptr<DescriptorArray> weak_array = bindings->array;
        bindings.reset(); // No logical/application owner remains before submission.
        const auto ticket = take(std::move(batch).submit());
        hold_observation = true;
        CHECK(take(queue.poll()) < ticket.serial());
        CHECK(!weak_bindings.expired() && !weak_input.expired() && !weak_indirect.expired() && !weak_image.expired());
        CHECK(binding_destructions == previous_bindings && buffer_destructions == previous_buffers);
        {
            auto table = weak_array.lock();
            auto unused = take(table->allocate());
            CHECK(unused.index() != retained_index); // In-flight descriptor is not reusable.
        }
        {
            auto rejected = take(queue.begin());
            auto rejected_range = std::make_shared<DescriptorRange>(take(array.allocate()));
            std::weak_ptr<DescriptorRange> weak_rejected = rejected_range;
            rejected.retain(rejected_range);
            rejected_range.reset();
            fail_submit = true;
            CHECK(!std::move(rejected).submit());
            CHECK(!fail_submit);
            CHECK(weak_rejected.expired() && array.occupied() == 1);
            CHECK(queue.lastSubmitted()->serial() == ticket.serial());
            CHECK(!weak_bindings.expired());
        }
        hold_observation = false;
        CHECK(take(queue.wait(ticket, UINT64_MAX)));
        CHECK(weak_bindings.expired() && weak_input.expired() && weak_indirect.expired() && weak_image.expired());
        CHECK(weak_array.expired());
        CHECK(binding_destructions == previous_bindings + 1);
        CHECK(buffer_destructions == previous_buffers + 2 && image_destructions == previous_images + 1);
        CHECK(array_destructions == previous_arrays + 1);
        std::array<std::array<std::uint32_t, 4>, 8> actual{};
        CHECK(output->read(0, std::as_writable_bytes(std::span(actual))));
        constexpr std::array<std::uint32_t, 4> expected_color{3, 7, 11, 19};
        for (std::size_t i = 0; i < input.size(); ++i)
            for (std::size_t channel = 0; channel < 4; ++channel)
                CHECK(actual[i][channel] == input[i] + expected_color[channel]);
        output.reset();
        CHECK(buffer_destructions == previous_buffers + 3);
    }
}

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        if (hold_observation && std::string_view(operation) == "fence_status") return VK_NOT_READY;
        if (fail_submit && std::string_view(operation) == "submit")
        {
            fail_submit = false;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char*, VkResult result) noexcept { return result; }

    void destroyed(const char* operation) noexcept
    {
        const std::string_view name(operation);
        if (name == "buffer") ++buffer_destructions;
        if (name == "image") ++image_destructions;
        if (name == "descriptor_array_pool") ++array_destructions;
    }
}

int main(int argc, char** argv)
{
    static_assert(!std::is_copy_constructible_v<DescriptorRange>);
    static_assert(std::is_nothrow_move_constructible_v<DescriptorRange>);
    CHECK(argc == 2);
    const auto spirv = readSpirv(argv[1]);
    Validation validation;
    auto bootstrap = instance(validation);
    {
        auto session = take(DeviceSession::create(bootstrap));
        auto invalid = session->allocator().createBuffer(64, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, EMemoryAccess::UPLOAD);
        CHECK(!invalid && invalid.error().type == kUnsupported);
    }
    for (unsigned topology = 0; topology < 3; ++topology)
    {
        DeviceOptions options;
        options.texture_descriptors = true;
        options.buffer_device_address = true;
        options.multiple_queues = topology != 0;
        options.prefer_dedicated_queues = topology != 2;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->device().bufferDeviceAddress());
        run(*session, spirv);
        std::printf("batch_resources topology=%u actual_queue=%u:%u descriptor_range_bytes=%zu output_words=32\n",
            topology, session->queue(EQueueRole::COMPUTE).nativeQueue().family,
            session->queue(EQueueRole::COMPUTE).nativeQueue().index, sizeof(DescriptorRange));
    }
    std::printf("validation_errors=%u warnings=%u bindings_destroyed=%u buffers_destroyed=%u images_destroyed=%u\n",
        validation.errors.load(), validation.warnings.load(), binding_destructions, buffer_destructions, image_destructions);
    CHECK(validation.errors == 0);
}
