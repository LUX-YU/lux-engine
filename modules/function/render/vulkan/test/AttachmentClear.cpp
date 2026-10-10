#include "F3Tonemap.pass.hpp"
#include "ShaderTestSupport.hpp"
#include <cstring>
#include <limits>
#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

using namespace foundation_test;
using namespace shader_test;
using Format = lux::rdesc::ETextureFormat;

static void run(
    const VulkanAllocator& allocator,
    SubmissionQueue& queue,
    Format format,
    VkFormat native_format,
    ColorClearValue clear,
    std::span<const std::byte> expected,
    const std::filesystem::path& output
)
{
    RenderGraphBuilder builder;
    F3Tonemap params;
    params.input.texture = builder.importTexture("input", {}, EPersistentScope::VIEW);
    params.nearest.sampler = GraphSampler{1};
    TextureDesc texture;
    texture.width = texture.height = 2;
    texture.format = format;
    params.output.texture = builder.importTexture("output", texture, EPersistentScope::VIEW);
    params.output.load = ELoadOp::CLEAR;
    params.output.clear = clear;
    CHECK(builder.graphics("clear", GraphicsShaderReference{{"F3Tonemap", "default"}}, EExecutionScope::VIEW, params));
    CHECK(builder.exportTexture(params.output.texture, PassProducer{passKey("clear")}));
    auto definition = take(std::move(builder).finish());
    auto plan = take(compileLogicalGraph(definition));
    auto invocation = makeGraphInvocationData(definition);
    const std::array imports{
        GraphImportBinding{GraphResourceId{1}, GraphBackingId{1}, 0, 1},
        GraphImportBinding{GraphResourceId{2}, GraphBackingId{2}, 0, 1}
    };
    CHECK(FrameGraphBindings::create(plan, {}, imports, invocation));
    const auto captured = std::get<ColorClearValue>(invocation.passes[0].fields.back());
    CHECK(captured == clear);
    auto image = take(Image::create(
        allocator,
        {2, 2},
        native_format,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    ));
    auto view = take(ImageView::create(image, VK_IMAGE_ASPECT_COLOR_BIT));
    auto readback =
        take(Buffer::create(allocator, expected.size() * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    auto command = take(queue.begin());
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
    attachment.clearValue.color = nativeColorClear(captured);
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {2, 2};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    // No draw is intentional: the oracle isolates the attachment load operation, not a fragment write.
    vkCmdBeginRendering(command.native(), &rendering);
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
    const auto serial = take(std::move(command).submit());
    CHECK(take(queue.wait(serial, 10'000'000'000ull)));
    std::vector<std::byte> bytes(expected.size() * 4);
    CHECK(readback.read(0, bytes));
    for (unsigned pixel = 0; pixel < 4; ++pixel)
    {
        CHECK(std::memcmp(bytes.data() + pixel * expected.size(), expected.data(), expected.size()) == 0);
    }
    std::ofstream(output, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::printf("G05 format=%d exact bytes=%zu attachment-load-clear\n", int(format), bytes.size());
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::filesystem::path evidence(argv[1]);
    std::filesystem::create_directories(evidence);
    Validation validation;
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto device = take(VulkanDevice::create(host, options));
        auto allocator = take(VulkanAllocator::create(device));
        auto queue = take(SubmissionQueue::create(device, 2));
        for (std::uint32_t value : {16777217u, 0xffffffffu})
        {
            run(allocator,
                *queue,
                Format::R32_UINT,
                VK_FORMAT_R32_UINT,
                UintColorClear{{value, 0, 0, 0}},
                std::as_bytes(std::span(&value, 1)),
                evidence / (std::to_string(value) + ".bin"));
        }
        const std::array uints{16777217u, 0xffffffffu, 7u, 0u};
        run(allocator,
            *queue,
            Format::RGBA32_UINT,
            VK_FORMAT_R32G32B32A32_UINT,
            UintColorClear{uints},
            std::as_bytes(std::span(uints)),
            evidence / "uint4.bin");
        const std::array sints{-1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), 0};
        run(allocator,
            *queue,
            Format::RGBA32_SINT,
            VK_FORMAT_R32G32B32A32_SINT,
            SintColorClear{sints},
            std::as_bytes(std::span(sints)),
            evidence / "sint4.bin");
        const std::array floats{2.5f, -1.5f, 0.5f, 1.0f};
        run(allocator,
            *queue,
            Format::RGBA32_SFLOAT,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            FloatColorClear{floats},
            std::as_bytes(std::span(floats)),
            evidence / "hdr.bin");
        const std::array<std::uint8_t, 4> srgb{0, 255, 0, 255};
        run(allocator,
            *queue,
            Format::RGBA8_SRGB,
            VK_FORMAT_R8G8B8A8_SRGB,
            FloatColorClear{{0, 1, 0, 1}},
            std::as_bytes(std::span(srgb)),
            evidence / "srgb.bin");
    }
    std::printf("VALIDATION errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors.load() == 0);
}
