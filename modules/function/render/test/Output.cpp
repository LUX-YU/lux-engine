#include "Support.hpp"
#include <array>
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/OffscreenTarget.hpp>
using namespace foundation_test;

static void copyBuffer(SubmissionQueue& queue, VkBuffer source, VkBuffer output, std::uint64_t bytes)
{
    auto batch = take(queue.begin());
    const VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(batch.native(), source, output, 1, &copy);
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    CHECK(std::move(batch).submit());
}

static void graphOutput(InstanceContext& bootstrap)
{
    auto session = take(DeviceSession::create(bootstrap));
    const TargetDesc target{16, 16, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    RenderGraphBuilder builder;
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
    const auto copied = take(builder.declareResource({"copied", GraphBufferDesc{1024}}));
    take(builder.addPass(
        {"clear",
         EGraphPassKind::TRANSFER,
         {{{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}
    ));
    take(builder.addPass(
        {"copy",
         EGraphPassKind::TRANSFER,
         {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
          {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}
    ));
    CHECK(builder.exportOutput({{color, 1}, {}, GraphOutputRequirements{target.format, target.samples}}));
    CHECK(builder.exportOutput({{pixels, 1}}));
    take(builder.addPass(
        {"copy-buffer",
         EGraphPassKind::TRANSFER,
         {{{pixels, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
          {{copied, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}
    ));
    CHECK(builder.exportOutput({{copied, 1}}));
    auto definition = take(std::move(builder).finish());
    auto analysis = take(analyze(definition));
    const std::array programs{PassProgram{"clear", {}, {1, 0, 1, 1}}, PassProgram{"copy"}, PassProgram{"copy-buffer"}};
    auto indexed = take(indexPrograms(definition, programs));
    auto graph = take(compileGraph(
        session->physicalDevice(),
        session->nativeDevice(),
        session->enabledFeatures(),
        session->queue().nativeQueue().flags,
        0,
        definition,
        analysis,
        {},
        indexed,
        target
    ));
    auto physical = take(OffscreenTarget::create(*session, target));
    auto execution = take(session->prepare(graph, *physical));
    const std::array contexts{execution.get()};
    auto output =
        take(session->allocator().createBuffer(1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    CHECK(take(session->render(contexts)) == 1);
    CHECK(take(session->render(contexts)) == 0);
    CHECK(session->drain());
    copyBuffer(session->queue(), execution->nativeBuffer(pixels.index), output.handle(), 1024);
    CHECK(session->drain());
    std::array<std::byte, 1024> bytes;
    CHECK(output.invalidate(session->allocator().native(), 0, bytes.size()) == VK_SUCCESS);
    std::ranges::copy(output.mappedBytes(session->allocator().native()), bytes.begin());
    for (std::size_t i = 0; i < bytes.size(); i += 4)
    {
        CHECK(bytes[i] == std::byte{255} && bytes[i + 1] == std::byte{0});
        CHECK(bytes[i + 2] == std::byte{255} && bytes[i + 3] == std::byte{255});
    }
    CHECK(take(session->render(contexts)) == 1);
    execution.reset(); // Physical resources outlive the business Context through Session cleanup.
    physical.reset();
    CHECK(session->drain());
    output.release(session->allocator().native());
    std::puts("Graph output: explicit external copy, correct pixels, Context detached in flight");
}

int main()
{
    Validation validation;
    {
        auto bootstrap = instance(validation);
        graphOutput(bootstrap);
    }
    CHECK(validation.errors == 0);
}
