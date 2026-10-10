#pragma once
#include "F3SharedA.pass.hpp"
#include "F3SharedB.pass.hpp"
#include "ShaderTestSupport.hpp"
#include <cstring>
#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

using namespace foundation_test;
using namespace shader_test;
using namespace lux::toolchain;

template <class T>
static RenderResult<NativeShaderProgram> program(
    const VulkanDevice& device,
    const std::filesystem::path& binaries,
    const std::filesystem::path& sources,
    std::span<const OwnerShape> owners
)
{
    const auto schema = PassSchema<T>::contract();
    const std::string name(schema.canonical_name);
    ShaderBuildInputs inputs{
        name,
        "default",
        "glslc VulkanSDK 1.4.304.0",
        "vulkan1.3",
        {{name + ".hpp", text(sources / (name + ".hpp"))},
         {"F3SharedValue.hpp", text(sources / "F3SharedValue.hpp")},
         {name + ".vert.lglsl", text(sources / (name + ".vert.lglsl"))},
         {name + ".frag.lglsl", text(sources / (name + ".frag.lglsl"))}},
        {}
    };
    auto variant = makeCompiledShaderVariant(
        std::move(inputs),
        {{1, words(binaries / (name + ".vert.spv"))}, {2, words(binaries / (name + ".frag.spv"))}},
        schema
    );
    if (!variant)
    {
        std::fprintf(stderr, "%s\n", variant.error().c_str());
    }
    CHECK(variant);
    RenderGraphBuilder builder;
    T params;
    const auto scene = builder.importBuffer("scene", {16, 16}, EPersistentScope::SCENE);
    const auto feature = builder.importBuffer("feature", {16, 16}, EPersistentScope::SCENE);
    if constexpr (std::is_same_v<T, F3SharedA>)
    {
        params.scene_a.buffer = scene;
        params.feature_a.buffer = feature;
    }
    else
    {
        params.scene_b.buffer = scene;
        params.feature_b.buffer = feature;
    }
    TextureDesc texture;
    texture.width = texture.height = 2;
    params.output.texture = builder.importTexture("output", texture, EPersistentScope::VIEW);
    const auto pass =
        take(builder.graphics("shared", GraphicsShaderReference{{name, "default"}}, EExecutionScope::VIEW, params));
    CHECK(builder.exportTexture(params.output.texture, PassProducer{passKey("shared")}));
    auto definition = take(std::move(builder).finish());
    auto graph = take(compileLogicalGraph(definition));
    GraphicsDescription graphics;
    graphics.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
    graphics.blends = {ColorBlend{}};
    auto wrong = graphics;
    wrong.color_formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    const OwnerAssignment assignment{owners[0].identity, owners[1].identity, {}};
    CHECK(!NativeShaderProgram::create(device, graph, pass, *variant, schema, assignment, owners, wrong));
    return NativeShaderProgram::create(device, graph, pass, *variant, schema, assignment, owners, graphics);
}

struct PendingSharedDraw
{
    Image image;
    ImageView view;
    Buffer readback;
    SubmissionTicket ticket;
};

static PendingSharedDraw submitSharedDraw(
    const VulkanAllocator& allocator,
    SubmissionQueue& queue,
    const NativeShaderProgram& program,
    const BoundDescriptorSets& descriptors,
    VkQueryPool timestamps = VK_NULL_HANDLE
)
{
    auto image = take(Image::create(
        allocator,
        {2, 2},
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    ));
    auto view = take(ImageView::create(image, VK_IMAGE_ASPECT_COLOR_BIT));
    auto readback = take(Buffer::create(allocator, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    auto command = take(queue.begin());
    if (timestamps)
    {
        vkCmdResetQueryPool(command.native(), timestamps, 0, 2);
        vkCmdWriteTimestamp2(command.native(), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps, 0);
    }
    imageBarrier(
        command.native(),
        image.native(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
    );
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = view.native();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {2, 2};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(command.native(), &rendering);
    const VkViewport viewport{0, 0, 2, 2, 0, 1};
    const VkRect2D scissor{{0, 0}, {2, 2}};
    vkCmdSetViewport(command.native(), 0, 1, &viewport);
    vkCmdSetScissor(command.native(), 0, 1, &scissor);
    vkCmdBindPipeline(command.native(), VK_PIPELINE_BIND_POINT_GRAPHICS, program.native());
    const std::vector<std::byte> scalars(program.identity().graph.passes[0].scalar_size);
    CHECK(descriptors.bind(command.native(), {}, scalars));
    vkCmdDraw(command.native(), 3, 1, 0, 0);
    vkCmdEndRendering(command.native());
    imageBarrier(
        command.native(),
        image.native(),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT
    );
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {2, 2, 1};
    vkCmdCopyImageToBuffer(
        command.native(),
        image.native(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.native(),
        1,
        &copy
    );
    memoryBarrier(
        command.native(),
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_HOST_BIT,
        VK_ACCESS_2_HOST_READ_BIT
    );
    if (timestamps)
    {
        vkCmdWriteTimestamp2(command.native(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps, 1);
    }
    const auto serial = take(std::move(command).submit());
    return {std::move(image), std::move(view), std::move(readback), serial};
}

static void checkSharedDraw(
    SubmissionQueue& queue,
    PendingSharedDraw& pending,
    F3SharedValue expected,
    const std::filesystem::path& output
)
{
    CHECK(take(queue.wait(pending.ticket, 10'000'000'000ull)));
    std::array<F3SharedValue, 4> pixels;
    CHECK(pending.readback.read(0, std::as_writable_bytes(std::span(pixels))));
    for (const auto& pixel : pixels)
    {
        CHECK(std::memcmp(&pixel, &expected, sizeof(pixel)) == 0);
    }
    std::ofstream(output, std::ios::binary).write(reinterpret_cast<const char*>(pixels.data()), sizeof(pixels));
}
