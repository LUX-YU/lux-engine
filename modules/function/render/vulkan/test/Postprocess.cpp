#include "F3Blur.pass.hpp"
#include "F3Composite.pass.hpp"
#include "F3Tonemap.pass.hpp"
#include "ShaderTestSupport.hpp"
#include "Support.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/vulkan/shader/Bindings.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>

using namespace foundation_test;
using namespace lux::toolchain;
using Pixels = std::vector<std::array<float, 4>>;
constexpr std::uint32_t width = 8, height = 8;

static std::vector<std::uint32_t> words(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input && input.tellg() > 0 && input.tellg() % 4 == 0);
    const auto size = static_cast<std::size_t>(input.tellg());
    std::vector<std::uint32_t> result(size / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.data()), size);
    CHECK(input);
    return result;
}

static std::string text(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    CHECK(input);
    return {std::istreambuf_iterator<char>(input), {}};
}

// Only test-owned single-queue transitions. These are not Graph compilation/recording mechanisms.
static void transition(
    VkCommandBuffer command,
    VkImage image,
    VkImageLayout before,
    VkImageLayout after,
    VkPipelineStageFlags2 source_stage,
    VkAccessFlags2 source_access,
    VkPipelineStageFlags2 destination_stage,
    VkAccessFlags2 destination_access
)
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = source_stage;
    barrier.srcAccessMask = source_access;
    barrier.dstStageMask = destination_stage;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);
}

static Image upload(const VulkanAllocator& allocator, SubmissionQueue& queue, const Pixels& data)
{
    auto image = take(Image::create(
        allocator,
        {width, height},
        VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    ));
    auto staging = take(Buffer::create(
        allocator,
        data.size() * sizeof(data[0]),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        EMemoryAccess::UPLOAD
    ));
    CHECK(staging.write(0, std::as_bytes(std::span(data))));
    auto command = take(queue.begin());
    transition(
        command.native(),
        image.native(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        0,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT
    );
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(
        command.native(),
        staging.native(),
        image.native(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy
    );
    transition(
        command.native(),
        image.native(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
    );
    const auto ticket = take(std::move(command).submit());
    CHECK(take(queue.wait(ticket, 10'000'000'000ull)));
    return image;
}

struct Output
{
    Image image;
    Pixels pixels;
};

template <class T>
static Output run(
    const VulkanDevice& device,
    const VulkanAllocator& allocator,
    SubmissionQueue& queue,
    const std::filesystem::path& binary_dir,
    const std::filesystem::path& source_dir,
    const std::filesystem::path& evidence_dir,
    std::span<const Image* const> inputs,
    float parameter,
    bool additive = false,
    bool srgb = false
)
{
    const auto schema = PassSchema<T>::contract();
    const std::string name(schema.canonical_name);
    const auto variant_name = additive ? "additive" : "default";
    ShaderBuildInputs build_inputs{
        name,
        variant_name,
        "glslc VulkanSDK 1.4.304.0",
        "vulkan1.3",
        {{"Fullscreen.vert.lglsl", text(source_dir / "Fullscreen.vert.lglsl")},
         {name + ".frag.lglsl", text(source_dir / (name + ".frag.lglsl"))},
         {name + ".hpp", text(source_dir / (name + ".hpp"))}},
        {}
    };
    if (additive)
    {
        build_inputs.defines.push_back({"F3_ADDITIVE", "1"});
    }
    std::vector stages{
        ShaderStageBinary{1, words(binary_dir / (name + ".vert.spv"))},
        ShaderStageBinary{2, words(binary_dir / (name + (additive ? ".additive.spv" : ".frag.spv")))}
    };
    auto variant = makeCompiledShaderVariant(std::move(build_inputs), std::move(stages), schema);
    if (!variant)
    {
        std::fprintf(stderr, "%s\n", variant.error().c_str());
    }
    CHECK(variant);
    RenderGraphBuilder builder;
    TextureDesc texture;
    texture.width = width;
    texture.height = height;
    T params;
    if constexpr (std::is_same_v<T, F3Tonemap>)
    {
        params.exposure = parameter;
        params.input.texture = builder.importTexture("input", texture, EPersistentScope::VIEW);
    }
    else if constexpr (std::is_same_v<T, F3Blur>)
    {
        params.strength = parameter;
        params.input[0].texture = builder.importTexture("input", texture, EPersistentScope::VIEW);
    }
    else
    {
        params.weight = parameter;
        params.inputs[0].texture = builder.importTexture("first", texture, EPersistentScope::VIEW);
        params.inputs[1].texture = builder.importTexture("second", texture, EPersistentScope::VIEW);
    }
    if (srgb)
    {
        texture.format = lux::rdesc::ETextureFormat::RGBA8_SRGB;
    }
    params.nearest.sampler = GraphSampler{1};
    params.output.texture = builder.importTexture("output", texture, EPersistentScope::VIEW);
    params.output.load = ELoadOp::CLEAR;
    params.output.clear = FloatColorClear{{-0.5f, -0.5f, -0.5f, -0.5f}};
    const auto pass =
        take(builder.graphics("post", GraphicsShaderReference{{name, variant_name}}, EExecutionScope::VIEW, params));
    CHECK(builder.exportTexture(params.output.texture, PassProducer{passKey("post")}));
    auto definition = take(std::move(builder).finish());
    auto graph = take(compileLogicalGraph(definition));
    auto invocation = makeGraphInvocationData(definition);
    const std::array schemas{schema};
    auto owner = take(declareOwnerShape("post.local", 1, lux::rdesc::EFieldOwner::PASS_LOCAL, schemas));
    const std::array owners{owner};
    const auto format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R32G32B32A32_SFLOAT;
    GraphicsDescription graphics;
    graphics.color_formats = {format};
    graphics.blends = {ColorBlend{}};
    auto program = take(
        NativeShaderProgram::create(device, graph, pass, *variant, schema, {{}, {}, owner.identity}, owners, graphics)
    );
    CHECK(program.identity().shader == variant->identity());
    shader_test::saveProgram(program, evidence_dir, name + "." + variant_name + (srgb ? ".srgb" : ".float"));
    // Exact cache comparison cannot confuse the compiled variant or native target state.
    auto different = program.identity();
    different.shader.inputs.variant_name += "other";
    CHECK(different != program.identity());
    auto sampler = take(Sampler::create(device, {}));
    std::vector<ImageView> views;
    views.reserve(inputs.size());
    for (const auto* input : inputs)
    {
        views.push_back(take(ImageView::create(*input, VK_IMAGE_ASPECT_COLOR_BIT)));
    }
    std::vector<OwnerDescriptorValue> values;
    const auto semantic = std::is_same_v<T, F3Composite> ? "inputs" : "input";
    for (std::uint32_t i = 0; i < views.size(); ++i)
    {
        values.push_back(
            {owner.identity,
             semantic,
             i,
             ImageDescriptorValue{std::cref(views[i]), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}
        );
    }
    values.push_back({owner.identity, "nearest", 0, std::cref(sampler)});
    auto descriptors = take(BoundDescriptorSets::create(device, program, values));
    auto incorrect = values;
    incorrect[0].element = 99;
    CHECK(!BoundDescriptorSets::create(device, program, incorrect));
    auto target = take(Image::create(
        allocator,
        {width, height},
        format,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    ));
    auto target_view = take(ImageView::create(target, VK_IMAGE_ASPECT_COLOR_BIT));
    const auto pixel_bytes = srgb ? 4u : 16u;
    auto readback = take(Buffer::create(
        allocator,
        width * height * pixel_bytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        EMemoryAccess::READBACK
    ));
    auto command = take(queue.begin());
    transition(
        command.native(),
        target.native(),
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
    );
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = target_view.native();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = nativeColorClear(std::get<ColorClearValue>(invocation.passes[0].fields.back()));
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {width, height};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(command.native(), &rendering);
    VkViewport viewport{0, 0, float(width), float(height), 0, 1};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(command.native(), 0, 1, &viewport);
    vkCmdSetScissor(command.native(), 0, 1, &scissor);
    vkCmdBindPipeline(command.native(), VK_PIPELINE_BIND_POINT_GRAPHICS, program.native());
    CHECK(descriptors.bind(command.native(), {}, invocation.passes[0].scalars));
    vkCmdDraw(command.native(), 3, 1, 0, 0);
    vkCmdEndRendering(command.native());
    transition(
        command.native(),
        target.native(),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT
    );
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(
        command.native(),
        target.native(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.native(),
        1,
        &copy
    );
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(command.native(), &dependency);
    transition(
        command.native(),
        target.native(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
    );
    const auto ticket = take(std::move(command).submit());
    CHECK(take(queue.wait(ticket, 10'000'000'000ull)));
    std::vector<std::byte> raw(width * height * pixel_bytes);
    CHECK(readback.read(0, raw));
    std::filesystem::create_directories(evidence_dir);
    const auto filename = name + "." + variant_name + (srgb ? ".srgb" : ".float");
    std::ofstream(evidence_dir / (filename + ".bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(raw.data()), raw.size());
    Pixels output(width * height);
    if (!srgb)
    {
        std::memcpy(output.data(), raw.data(), raw.size());
    }
    else
    {
        for (std::size_t i = 0; i < output.size(); ++i)
        {
            for (std::size_t channel = 0; channel < 4; ++channel)
            {
                output[i][channel] = std::to_integer<unsigned char>(raw[i * 4 + channel]) / 255.0f;
            }
        }
    }
    std::printf(
        "GPU %s submitted=%llu completed=%llu pixels=%zu\n",
        filename.c_str(),
        static_cast<unsigned long long>(ticket.serial()),
        static_cast<unsigned long long>(queue.completed()),
        output.size()
    );
    return {std::move(target), std::move(output)};
}

static void compare(const Pixels& observed, const Pixels& reference, float tolerance, const char* name)
{
    float maximum_error = 0;
    CHECK(observed.size() == reference.size());
    for (std::size_t pixel = 0; pixel < observed.size(); ++pixel)
    {
        for (unsigned channel = 0; channel < 4; ++channel)
        {
            const float error = std::abs(observed[pixel][channel] - reference[pixel][channel]);
            if (!std::isfinite(error) || error > tolerance)
            {
                std::fprintf(
                    stderr,
                    "%s pixel=%zu channel=%u actual=%g reference=%g\n",
                    name,
                    pixel,
                    channel,
                    observed[pixel][channel],
                    reference[pixel][channel]
                );
            }
            CHECK(std::isfinite(error) && error <= tolerance);
            maximum_error = std::max(maximum_error, error);
        }
    }
    std::printf("ORACLE %s max_error=%g tolerance=%g\n", name, maximum_error, tolerance);
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CHECK(argc == 4);
    const std::filesystem::path binary_dir(argv[1]), source_dir(argv[2]), evidence_dir(argv[3]);
    Validation validation;
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto device = take(VulkanDevice::create(host, options));
        auto allocator = take(VulkanAllocator::create(device));
        auto queue = take(SubmissionQueue::create(device, 3));
        std::printf(
            "DEVICE %s driver=%u API=%u dynamic_rendering=%d\n",
            device.properties().deviceName,
            device.properties().driverVersion,
            device.properties().apiVersion,
            device.dynamicRendering()
        );
        Pixels source(width * height), tone_reference(width * height), blur_reference(width * height),
            composite_reference(width * height);
        constexpr float exposure = 1.7f, strength = 0.8f, weight = 0.35f;
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            source[i] = {float(i % width) * 0.45f, float(i / width) * 0.37f, float((i * 13) % 17) * 0.31f, 1};
            for (unsigned c = 0; c < 3; ++c)
            {
                tone_reference[i][c] = source[i][c] * exposure / (1 + source[i][c] * exposure);
            }
            tone_reference[i][3] = 1;
        }
        auto uploaded = upload(allocator, *queue, source);
        const std::array first{&uploaded};
        auto tone = run<F3Tonemap>(device, allocator, *queue, binary_dir, source_dir, evidence_dir, first, exposure);
        compare(tone.pixels, tone_reference, 0.00002f, "Tonemap HDR");
        auto srgb = run<
            F3Tonemap>(device, allocator, *queue, binary_dir, source_dir, evidence_dir, first, exposure, false, true);
        auto srgb_reference = tone_reference;
        for (auto& pixel : srgb_reference)
        {
            for (unsigned c = 0; c < 3; ++c)
            {
                pixel[c] = pixel[c] <= 0.0031308f ? pixel[c] * 12.92f : 1.055f * std::pow(pixel[c], 1 / 2.4f) - 0.055f;
            }
        }
        compare(srgb.pixels, srgb_reference, 1.1f / 255, "Tonemap sRGB");
        for (int y = 0; y < int(height); ++y)
        {
            for (int x = 0; x < int(width); ++x)
            {
                for (unsigned c = 0; c < 4; ++c)
                {
                    float sum = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            sum += tone_reference
                                [std::clamp(y + dy, 0, int(height) - 1) * width + std::clamp(x + dx, 0, int(width) - 1)]
                                [c];
                        }
                    }
                    blur_reference[y * width + x][c] =
                        tone_reference[y * width + x][c] * (1 - strength) + sum / 9 * strength;
                }
            }
        }
        const std::array second{&tone.image};
        auto blur = run<F3Blur>(device, allocator, *queue, binary_dir, source_dir, evidence_dir, second, strength);
        compare(blur.pixels, blur_reference, 0.00002f, "Blur neighborhood");
        const std::array pair{&tone.image, &blur.image};
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            for (unsigned c = 0; c < 4; ++c)
            {
                composite_reference[i][c] = tone_reference[i][c] * (1 - weight) + blur_reference[i][c] * weight;
            }
        }
        auto composite =
            run<F3Composite>(device, allocator, *queue, binary_dir, source_dir, evidence_dir, pair, weight);
        compare(composite.pixels, composite_reference, 0.00002f, "Composite two inputs");
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            for (unsigned c = 0; c < 4; ++c)
            {
                composite_reference[i][c] = tone_reference[i][c] + blur_reference[i][c] * weight;
            }
        }
        auto additive =
            run<F3Composite>(device, allocator, *queue, binary_dir, source_dir, evidence_dir, pair, weight, true);
        compare(additive.pixels, composite_reference, 0.00002f, "Composite compiled variant");
    }
    std::printf("VALIDATION errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors.load() == 0);
}
