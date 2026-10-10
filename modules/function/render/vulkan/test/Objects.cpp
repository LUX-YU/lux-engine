#include "Support.hpp"

#include <lux/engine/render/vulkan/memory/Memory.hpp>
#include <lux/engine/render/vulkan/descriptor/Descriptors.hpp>
#include <lux/engine/render/vulkan/pipeline/Pipeline.hpp>
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
        CHECK(!Buffer::create(allocator, 0, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
        CHECK(!Image::create(allocator, {0, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT));
        auto first = take(Buffer::create(allocator, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, EMemoryAccess::READBACK));
        auto second = take(Buffer::create(allocator, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, EMemoryAccess::READBACK));
        first = std::move(second);
        CHECK(!second.native());
        std::array<std::byte, 256> values{};
        values.fill(std::byte{0x75});
        CHECK(first.write(0, values));
        std::array<std::byte, 256> output{};
        CHECK(first.read(0, output) && output == values);
        CHECK(!first.write(1, values));
        CHECK(!first.read(~VkDeviceSize{0}, output));
        auto candidate = Buffer::create(allocator, 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, EMemoryAccess::DEVICE);
        CHECK(!candidate && first.size() == 256);
        auto image = take(Image::create(
            allocator,
            {8, 8},
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
        ));
        auto moved_image = std::move(image);
        CHECK(!image.native() && moved_image.extent().width == 8);
        const std::array bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
        };
        auto descriptor_layout = take(DescriptorSetLayout::create(device, bindings));
        const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        auto pool = take(DescriptorPool::create(device, 1, sizes));
        auto set = take(pool.allocate(descriptor_layout));
        auto exhausted = pool.allocate(descriptor_layout);
        CHECK(!exhausted && exhausted.error().type == kCapacity);
        CHECK(pool.writeStorageBuffer(set, 0, first, 0, first.size()));
        CHECK(!pool.writeStorageBuffer(set, 0, first, 1, first.size()));
        auto moved_pool = std::move(pool);
        CHECK(!pool.native() && moved_pool.native());
        const std::array layouts{descriptor_layout.native()};
        auto layout = take(PipelineLayout::create(device, layouts));
        auto shader = take(ShaderModule::create(device, spirv));
        CHECK(!ShaderModule::create(device, {}));
        auto pipeline = take(ComputePipeline::create(device, shader, layout));
        auto replacement = take(ComputePipeline::create(device, shader, layout));
        pipeline = std::move(replacement);
        CHECK(!replacement.native() && pipeline.native());
    }
    CHECK(validation.errors == 0);
    std::printf(
        "PASS memory/pipeline validation_errors=%u validation_warnings=%u\n",
        validation.errors.load(),
        validation.warnings.load()
    );
}
