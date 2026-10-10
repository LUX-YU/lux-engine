#include "F3Raster.pass.hpp"
#include "ShaderTestSupport.hpp"
#include <cstring>
#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

using namespace foundation_test;
using namespace shader_test;
using namespace lux::toolchain;

static void run(
    const VulkanDevice& device,
    const VulkanAllocator& allocator,
    SubmissionQueue& queue,
    const CompiledShaderVariant& variant,
    VkSampleCountFlagBits samples,
    const std::filesystem::path& evidence
)
{
    const auto schema = PassSchema<F3Raster>::contract();
    RenderGraphBuilder builder;
    TextureDesc texture;
    texture.width = texture.height = 4;
    texture.samples = samples;
    F3Raster params;
    params.output.texture = builder.importTexture("color", texture, EPersistentScope::VIEW);
    params.output.clear = FloatColorClear{{0, 0, 0, 1}};
    texture.format = lux::rdesc::ETextureFormat::D32_SFLOAT_S8_UINT;
    params.depth.texture = builder.importTexture("depth", texture, EPersistentScope::VIEW);
    params.depth.range.aspect = EAspect::DEPTH_STENCIL;
    params.depth.stencil_load = ELoadOp::CLEAR;
    params.depth.stencil_store = EStoreOp::STORE;
    params.depth.clear_stencil = 1;
    const auto pass =
        take(builder.graphics("raster", GraphicsShaderReference{{"F3Raster", "default"}}, EExecutionScope::VIEW, params)
        );
    CHECK(builder.exportTexture(params.output.texture, PassProducer{passKey("raster")}));
    auto definition = take(std::move(builder).finish());
    auto graph = take(compileLogicalGraph(definition));
    auto invocation = makeGraphInvocationData(definition);
    GraphicsDescription description;
    description.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
    ColorBlend blend;
    blend.enabled = true;
    blend.source_color = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.destination_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    description.blends = {blend};
    description.samples = samples;
    description.depth_format = description.stencil_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    description.depth_test = description.depth_write = description.stencil_test = true;
    description.front.compare = description.back.compare = VK_COMPARE_OP_EQUAL;
    description.front.pass = description.back.pass = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    description.front.reference = description.back.reference = 1;
    auto first = take(NativeShaderProgram::create(device, graph, pass, variant, schema, {}, {}, description));
    description.front.reference = description.back.reference = 2;
    auto second = take(NativeShaderProgram::create(device, graph, pass, variant, schema, {}, {}, description));
    CHECK(first.identity() != second.identity());
    saveProgram(first, evidence, "raster-" + std::to_string(samples));
    auto bad = description;
    bad.stencil_format = VK_FORMAT_D32_SFLOAT;
    CHECK(!NativeShaderProgram::create(device, graph, pass, variant, schema, {}, {}, bad));
    auto first_sets = take(BoundDescriptorSets::create(device, first, {}));
    auto second_sets = take(BoundDescriptorSets::create(device, second, {}));
    auto color = take(Image::create(
        allocator,
        {4, 4},
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        samples
    ));
    auto depth = take(Image::create(
        allocator,
        {4, 4},
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        samples
    ));
    auto resolved = take(Image::create(
        allocator,
        {4, 4},
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    ));
    auto color_view = take(ImageView::create(color, VK_IMAGE_ASPECT_COLOR_BIT));
    auto depth_view = take(ImageView::create(depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    auto resolve_view = take(ImageView::create(resolved, VK_IMAGE_ASPECT_COLOR_BIT));
    auto readback = take(Buffer::create(allocator, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    auto command = take(queue.begin());
    for (auto image : {color.native(), resolved.native()})
    {
        imageBarrier(
            command.native(),
            image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_NONE,
            0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
        );
    }
    imageBarrier(
        command.native(),
        depth.native(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        0,
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
    );
    VkRenderingAttachmentInfo color_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color_attachment.imageView = color_view.native();
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = nativeColorClear(std::get<ColorClearValue>(invocation.passes[0].fields[0]));
    if (samples != VK_SAMPLE_COUNT_1_BIT)
    {
        color_attachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color_attachment.resolveImageView = resolve_view.native();
        color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_attachment.imageView = depth_view.native();
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    const auto clear = std::get<DepthStencilClearValue>(invocation.passes[0].fields[1]);
    depth_attachment.clearValue.depthStencil = {clear.depth, clear.stencil};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {4, 4};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color_attachment;
    rendering.pDepthAttachment = rendering.pStencilAttachment = &depth_attachment;
    vkCmdBeginRendering(command.native(), &rendering);
    const VkViewport viewport{0, 0, 4, 4, 0, 1};
    const VkRect2D scissor{{0, 0}, {4, 4}};
    vkCmdSetViewport(command.native(), 0, 1, &viewport);
    vkCmdSetScissor(command.native(), 0, 1, &scissor);
    const auto draw =
        [&](const NativeShaderProgram& program, const BoundDescriptorSets& sets, std::array<float, 5> values)
    {
        // Generated ABI checks fix these five contiguous scalar offsets; Invocation owns the changing bytes.
        static_assert(offsetof(F3Raster, red) == 0 && offsetof(F3Raster, depth_value) == 16);
        std::memcpy(invocation.passes[0].scalars.data(), values.data(), sizeof(values));
        vkCmdBindPipeline(command.native(), VK_PIPELINE_BIND_POINT_GRAPHICS, program.native());
        CHECK(sets.bind(command.native(), {}, invocation.passes[0].scalars));
        vkCmdDraw(command.native(), 3, 1, 0, 0);
    };
    draw(first, first_sets, {1, 0, 0, 0.5f, 0.4f});   // Passes, stencil becomes 2.
    draw(first, first_sets, {0, 1, 0, 1, 0.2f});      // Stencil rejects.
    draw(second, second_sets, {0, 0, 1, 1, 0.8f});    // Depth rejects.
    draw(second, second_sets, {1, 1, 0, 0.5f, 0.2f}); // Passes and blends.
    vkCmdEndRendering(command.native());
    const auto output = samples == VK_SAMPLE_COUNT_1_BIT ? color.native() : resolved.native();
    imageBarrier(
        command.native(),
        output,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT
    );
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {4, 4, 1};
    vkCmdCopyImageToBuffer(command.native(), output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.native(), 1, &copy);
    memoryBarrier(
        command.native(),
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT,
        VK_ACCESS_2_HOST_READ_BIT
    );
    const auto serial = take(std::move(command).submit());
    CHECK(take(queue.wait(serial, 10'000'000'000ull)));
    std::array<std::array<float, 4>, 16> pixels;
    CHECK(readback.read(0, std::as_writable_bytes(std::span(pixels))));
    const std::array<float, 4> expected{0.75f, 0.5f, 0, 0.5f};
    for (const auto& pixel : pixels)
    {
        CHECK(pixel == expected);
    }
    std::ofstream(evidence / ("samples-" + std::to_string(samples) + ".bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(pixels.data()), sizeof(pixels));
    std::printf(
        "G06 samples=%u depth/stencil reject, blend; resolve=%d; pixels exact\n",
        unsigned(samples),
        samples != VK_SAMPLE_COUNT_1_BIT
    );
}

int main(int argc, char** argv)
{
    CHECK(argc == 4);
    const std::filesystem::path binaries(argv[1]), sources(argv[2]), evidence(argv[3]);
    std::filesystem::create_directories(evidence);
    const auto schema = PassSchema<F3Raster>::contract();
    ShaderBuildInputs inputs{
        "F3Raster",
        "default",
        "glslc VulkanSDK 1.4.304.0",
        "vulkan1.3",
        {{"F3Raster.hpp", text(sources / "F3Raster.hpp")},
         {"F3Raster.vert.lglsl", text(sources / "F3Raster.vert.lglsl")},
         {"F3Raster.frag.lglsl", text(sources / "F3Raster.frag.lglsl")}},
        {}
    };
    auto variant = makeCompiledShaderVariant(
        std::move(inputs),
        {{1, words(binaries / "F3Raster.vert.spv")}, {2, words(binaries / "F3Raster.frag.spv")}},
        schema
    );
    if (!variant)
    {
        std::fprintf(stderr, "%s\n", variant.error().c_str());
    }
    CHECK(variant);
    Validation validation;
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto device = take(VulkanDevice::create(host, options));
        auto allocator = take(VulkanAllocator::create(device));
        auto queue = take(SubmissionQueue::create(device, 2));
        run(device, allocator, *queue, *variant, VK_SAMPLE_COUNT_1_BIT, evidence);
        run(device, allocator, *queue, *variant, VK_SAMPLE_COUNT_4_BIT, evidence);
    }
    std::printf("VALIDATION errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors.load() == 0);
}
