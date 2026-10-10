#include "F3Storage.pass.hpp"
#include "ShaderTestSupport.hpp"
#include <algorithm>
#include <cstring>
#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

using namespace foundation_test;
using namespace shader_test;
using namespace lux::toolchain;

int main(int argc, char** argv)
{
    CHECK(argc == 4);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::filesystem::path binary_dir(argv[1]), source_dir(argv[2]), evidence_dir(argv[3]);
    Validation validation;
    {
        auto host = instance(validation);
        auto device = take(VulkanDevice::create(host));
        auto allocator = take(VulkanAllocator::create(device));
        auto queue = take(SubmissionQueue::create(device, 3));
        const auto schema = PassSchema<F3Storage>::contract();
        ShaderBuildInputs inputs{
            "F3Storage",
            "default",
            "glslc VulkanSDK 1.4.304.0",
            "vulkan1.3",
            {{"F3Storage.comp.lglsl", text(source_dir / "F3Storage.comp.lglsl")},
             {"F3Storage.hpp", text(source_dir / "F3Storage.hpp")}},
            {}
        };
        auto variant =
            makeCompiledShaderVariant(std::move(inputs), {{4, words(binary_dir / "F3Storage.comp.spv")}}, schema);
        if (!variant)
        {
            std::fprintf(stderr, "%s\n", variant.error().c_str());
        }
        CHECK(variant);
        RenderGraphBuilder builder;
        F3Storage params;
        params.config.buffer = builder.importBuffer("config", {256, 16}, EPersistentScope::SCENE);
        params.config.range = {0, 16};
        params.input.buffer = builder.importBuffer("input", {256, 16}, EPersistentScope::SCENE);
        params.input.range = {0, 64};
        params.outputs[0].buffer = builder.importBuffer("output.first", {256, 16}, EPersistentScope::VIEW);
        params.outputs[0].range = {0, 64};
        params.outputs[1].buffer = builder.importBuffer("output.second", {256, 16}, EPersistentScope::VIEW);
        params.outputs[1].range = {0, 64};
        TextureDesc image_description;
        image_description.width = 4;
        params.image.texture = builder.importTexture("image", image_description, EPersistentScope::VIEW);
        const auto pass = take(
            builder.compute("storage", ComputeShaderReference{{"F3Storage", "default"}}, EExecutionScope::VIEW, params)
        );
        CHECK(builder.exportTexture(params.image.texture, PassProducer{passKey("storage")}));
        auto definition = take(std::move(builder).finish());
        auto graph = take(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        const std::array schemas{schema};
        auto scene = take(declareOwnerShape("a.scene", 1, lux::rdesc::EFieldOwner::SCENE, schemas));
        auto feature = take(declareOwnerShape("b.feature", 1, lux::rdesc::EFieldOwner::FEATURE, schemas));
        auto local = take(declareOwnerShape("c.local", 1, lux::rdesc::EFieldOwner::PASS_LOCAL, schemas));
        scene.fields[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        feature.fields[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        const std::array owners{scene, feature, local};
        auto program = take(NativeShaderProgram::create(
            device,
            graph,
            pass,
            *variant,
            schema,
            {scene.identity, feature.identity, local.identity},
            owners,
            ComputeDescription{}
        ));
        CHECK(program.identity().layout.sets.size() == 2 && program.identity().layout.dynamic_count == 2);
        {
            for (const auto capability : {11u, 40u})
            {
                auto stages = variant->identity().stages;
                // shaderInt64 and dynamic-rendering local read are deliberately not enabled.
                stages[0].words.insert(stages[0].words.begin() + 5, {(2u << 16) | 17u, capability});
                auto unsupported = makeCompiledShaderVariant(variant->identity().inputs, stages, schema);
                CHECK(unsupported);
                auto rejected = NativeShaderProgram::create(
                    device,
                    graph,
                    pass,
                    *unsupported,
                    schema,
                    {scene.identity, feature.identity, local.identity},
                    owners,
                    ComputeDescription{}
                );
                CHECK(!rejected && rejected.error().type == kUnsupported);
            }
            auto stages = variant->identity().stages;
            bool changed_group = false;
            for (std::size_t i = 5; i < stages[0].words.size(); i += stages[0].words[i] >> 16)
            {
                if ((stages[0].words[i] & 0xffff) == 16 && stages[0].words[i + 2] == 17)
                {
                    stages[0].words[i + 3] = device.properties().limits.maxComputeWorkGroupSize[0] + 1;
                    changed_group = true;
                }
            }
            CHECK(changed_group);
            auto unsupported = makeCompiledShaderVariant(variant->identity().inputs, stages, schema);
            CHECK(unsupported);
            auto too_large = NativeShaderProgram::create(
                device,
                graph,
                pass,
                *unsupported,
                schema,
                {scene.identity, feature.identity, local.identity},
                owners,
                ComputeDescription{}
            );
            CHECK(!too_large && too_large.error().type == kUnsupported);
        }
        saveProgram(program, evidence_dir, "compute");
        const auto config_offset = device.properties().limits.minUniformBufferOffsetAlignment;
        const auto input_offset =
            std::max<VkDeviceSize>(16, device.properties().limits.minStorageBufferOffsetAlignment);
        CHECK(config_offset + 16 <= 256 && input_offset + 64 <= 256);
        auto config = take(Buffer::create(allocator, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, EMemoryAccess::UPLOAD));
        auto input = take(Buffer::create(allocator, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, EMemoryAccess::UPLOAD));
        const F3Vector weights{10, 20, 30, 40};
        const std::array
            data{F3Vector{1, 2, 3, 4}, F3Vector{5, 6, 7, 8}, F3Vector{9, 10, 11, 12}, F3Vector{13, 14, 15, 16}};
        CHECK(config.write(config_offset, std::as_bytes(std::span(&weights, 1))));
        CHECK(input.write(input_offset, std::as_bytes(std::span(data))));
        const auto output_usage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto output0 = take(Buffer::create(allocator, 256, output_usage, EMemoryAccess::DEVICE));
        auto output1 = take(Buffer::create(allocator, 256, output_usage, EMemoryAccess::DEVICE));
        auto image = take(Image::create(
            allocator,
            {4, 1},
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
        ));
        auto view = take(ImageView::create(image, VK_IMAGE_ASPECT_COLOR_BIT));
        std::vector<OwnerDescriptorValue> values{
            {scene.identity, "config", 0, BufferDescriptorValue{std::cref(config), 0, 16}},
            {feature.identity, "input", 0, BufferDescriptorValue{std::cref(input), 0, 64}},
            {local.identity, "outputs", 0, BufferDescriptorValue{std::cref(output0), 0, 64}},
            {local.identity, "outputs", 1, BufferDescriptorValue{std::cref(output1), 64, 64}},
            {local.identity, "image", 0, ImageDescriptorValue{std::cref(view), VK_IMAGE_LAYOUT_GENERAL}}
        };
        auto descriptors = take(BoundDescriptorSets::create(device, program, values));
        auto invalid = values;
        std::get<BufferDescriptorValue>(invalid[0].value).offset = 1;
        CHECK(!BoundDescriptorSets::create(device, program, invalid));
        invalid = values;
        std::get<BufferDescriptorValue>(invalid[0].value).range = 8;
        CHECK(!BoundDescriptorSets::create(device, program, invalid));
        invalid = values;
        invalid[3].element = 0;
        CHECK(!BoundDescriptorSets::create(device, program, invalid));
        auto readback = take(Buffer::create(allocator, 192, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
        auto command = take(queue->begin());
        vkCmdFillBuffer(command.native(), output0.native(), 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(command.native(), output1.native(), 0, VK_WHOLE_SIZE, 0);
        memoryBarrier(
            command.native(),
            VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
        );
        imageBarrier(
            command.native(),
            image.native(),
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_NONE,
            0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
        );
        const std::array offsets{static_cast<std::uint32_t>(config_offset), static_cast<std::uint32_t>(input_offset)};
        const std::array misaligned{1u, static_cast<std::uint32_t>(input_offset)};
        const std::array outside{4096u, static_cast<std::uint32_t>(input_offset)};
        CHECK(!descriptors.bind(command.native(), misaligned, invocation.passes[0].scalars));
        CHECK(!descriptors.bind(command.native(), outside, invocation.passes[0].scalars));
        CHECK(!descriptors.bind(command.native(), {}, invocation.passes[0].scalars));
        vkCmdBindPipeline(command.native(), VK_PIPELINE_BIND_POINT_COMPUTE, program.native());
        CHECK(descriptors.bind(command.native(), offsets, invocation.passes[0].scalars));
        vkCmdDispatch(command.native(), 4, 1, 1);
        memoryBarrier(
            command.native(),
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT
        );
        VkBufferCopy first{0, 0, 64}, second{64, 64, 64};
        vkCmdCopyBuffer(command.native(), output0.native(), readback.native(), 1, &first);
        vkCmdCopyBuffer(command.native(), output1.native(), readback.native(), 1, &second);
        imageBarrier(
            command.native(),
            image.native(),
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT
        );
        VkBufferImageCopy image_copy{};
        image_copy.bufferOffset = 128;
        image_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        image_copy.imageExtent = {4, 1, 1};
        vkCmdCopyImageToBuffer(
            command.native(),
            image.native(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback.native(),
            1,
            &image_copy
        );
        memoryBarrier(
            command.native(),
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT
        );
        const auto ticket = take(std::move(command).submit());
        CHECK(take(queue->wait(ticket, 10'000'000'000ull)));
        std::array<F3Vector, 12> result;
        CHECK(readback.read(0, std::as_writable_bytes(std::span(result))));
        for (unsigned i = 0; i < 4; ++i)
        {
            const float value = data[i].x + data[i].y + weights.x + weights.w;
            CHECK(
                result[i].x == value && result[i].y == value + 1 && result[i].z == value + 2 && result[i].w == value + 3
            );
            CHECK(
                result[i + 4].x == value * 2 && result[i + 4].y == value * 3 && result[i + 4].z == value * 4 &&
                result[i + 4].w == value * 5
            );
            CHECK(std::memcmp(&result[i], &result[i + 8], sizeof(F3Vector)) == 0);
        }
        std::filesystem::create_directories(evidence_dir);
        std::ofstream(evidence_dir / "storage.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(result.data()), sizeof(result));
        std::printf(
            "G02/G04 compute values exact; UBO-offset=%llu SSBO-offset=%llu sets=2 array=2 stride=16 PC-count=4\n",
            static_cast<unsigned long long>(config_offset),
            static_cast<unsigned long long>(input_offset)
        );
    }
    std::printf("VALIDATION errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors.load() == 0);
}
