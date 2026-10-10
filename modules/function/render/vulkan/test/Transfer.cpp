#include "Support.hpp"

#include <lux/engine/render/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/vulkan/transfer/Transfer.hpp>
#include <array>
#include <fstream>
#include <vector>

using namespace foundation_test;

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    CHECK(input);
    const auto bytes = static_cast<std::size_t>(input.tellg());
    CHECK(bytes % 4 == 0);
    std::vector<std::uint32_t> spirv(bytes / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char *>(spirv.data()), static_cast<std::streamsize>(bytes));
    CHECK(input);
    Validation validation;
    {
        auto instance_owner = instance(validation);
        auto device = take(VulkanDevice::create(instance_owner));
        auto allocator = take(VulkanAllocator::create(device));
        auto queue = take(SubmissionQueue::create(device, 3));
        auto arena = take(StagingArena::create(*queue, device, allocator, 65536));
        auto retired = take(RetirementQueue::create(*queue, 8));
        auto gpu = take(Buffer::create(
            allocator,
            1024,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            EMemoryAccess::DEVICE
        ));
        auto readback =
            take(Buffer::create(allocator, 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
        auto image = take(Image::create(
            allocator,
            {8, 8},
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        ));
        auto image_readback =
            take(Buffer::create(allocator, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
        const std::array bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
        };
        auto descriptor_layout = take(DescriptorSetLayout::create(device, bindings));
        const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        auto pool = take(DescriptorPool::create(device, 1, sizes));
        auto set = take(pool.allocate(descriptor_layout));
        CHECK(pool.writeStorageBuffer(set, 0, gpu, 0, gpu.size()));
        const std::array layouts{descriptor_layout.native()};
        auto layout = take(PipelineLayout::create(device, layouts));
        auto shader = take(ShaderModule::create(device, spirv));
        auto pipeline = take(ComputePipeline::create(device, shader, layout));
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i)
            values[i] = i;
        std::array<std::byte, 256> pixels{};
        for (std::size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = std::byte(i);
        // Repeat on the same native backing. No new scene/publication or pipeline
        // construction is required between these independent native submissions.
        for (int iteration = 0; iteration < 12; ++iteration)
        {
            auto batch = take(queue->begin());
            auto staged = take(arena.stage(batch, std::as_bytes(std::span{values})));
            CHECK(recordUpload(batch, staged, gpu));
            vkCmdBindPipeline(batch.native(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.native());
            vkCmdBindDescriptorSets(
                batch.native(), VK_PIPELINE_BIND_POINT_COMPUTE, layout.native(), 0, 1, &set, 0, nullptr
            );
            vkCmdDispatch(batch.native(), 4, 1, 1);
            CHECK(recordBufferCopy(batch, gpu, readback, 1024, 0, 0, true));
            auto image_staged = take(arena.stage(batch, pixels));
            CHECK(recordImageUpload(
                batch, image_staged, image, iteration == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL
            ));
            CHECK(recordImageReadback(batch, image, VK_IMAGE_LAYOUT_GENERAL, image_readback));
            auto ticket = take(std::move(batch).submit());
            if (iteration == 11)
            {
                // Publication ends before transfer of ownership. Native backing
                // remains in the retirement owner until the last-use fence.
                CHECK(retired.retire(ticket, std::move(gpu)));
                CHECK(retired.retire(ticket, std::move(image)));
                CHECK(!gpu.native() && !image.native());
            }
            CHECK(take(queue->wait(ticket, 5'000'000'000)));
            std::array<std::uint32_t, 256> output{};
            CHECK(readback.read(0, std::as_writable_bytes(std::span{output})));
            for (std::uint32_t i = 0; i < output.size(); ++i)
                CHECK(output[i] == i * 3 + 7);
            std::array<std::byte, 256> pixel_output{};
            CHECK(image_readback.read(0, pixel_output));
            CHECK(pixel_output == pixels);
        }
        CHECK(take(retired.collect()) == 2);
        CHECK(retired.pending() == 0 && queue->completed() == 12);
    }
    CHECK(validation.errors == 0);
    std::printf(
        "PASS compute/buffer/image readback submissions=12 validation_errors=%u validation_warnings=%u\n",
        validation.errors.load(),
        validation.warnings.load()
    );
}
