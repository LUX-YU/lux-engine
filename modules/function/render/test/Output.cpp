#include "Support.hpp"
#include <array>
#include <cstring>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/graph/Builder.hpp>
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
    auto session = take(DeviceSession::create(bootstrap, {}, 3, 3));
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
    const auto device = session->nativeDevice();
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = 1024;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer output{};
    CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &output) == VK_SUCCESS);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, output, &requirements);
    const auto& memory_types = session->physicalDevice().memoryProperties().memoryProperties;
    std::uint32_t memory_type = UINT32_MAX;
    for (std::uint32_t i = 0; i < memory_types.memoryTypeCount; ++i)
    {
        const bool supported = (requirements.memoryTypeBits & (1u << i)) != 0;
        const bool visible = (memory_types.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        if (supported && visible)
        {
            memory_type = i;
            break;
        }
    }
    CHECK(memory_type != UINT32_MAX);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    VkDeviceMemory memory{};
    CHECK(vkAllocateMemory(device, &allocation, nullptr, &memory) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, output, memory, 0) == VK_SUCCESS);
    CHECK(take(session->render(contexts)) == 1);
    CHECK(take(session->render(contexts)) == 1);
    CHECK(take(session->render(contexts)) == 1);
    CHECK(take(session->render(contexts)) == 0);
    CHECK(session->drain());
    copyBuffer(
        session->queue(),
        execution->nativeBuffer(pixels.index, execution->progress().last_submitted_slot),
        output,
        1024
    );
    CHECK(session->drain());
    std::array<std::byte, 1024> bytes;
    void* mapped = nullptr;
    CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS);
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &range) == VK_SUCCESS);
    std::memcpy(bytes.data(), mapped, bytes.size());
    vkUnmapMemory(device, memory);
    for (std::size_t i = 0; i < bytes.size(); i += 4)
    {
        CHECK(bytes[i] == std::byte{255} && bytes[i + 1] == std::byte{0});
        CHECK(bytes[i + 2] == std::byte{255} && bytes[i + 3] == std::byte{255});
    }
    CHECK(take(session->render(contexts)) == 1);
    execution.reset(); // Physical resources outlive the business Context through Session cleanup.
    physical.reset();
    CHECK(session->drain());
    vkDestroyBuffer(device, output, nullptr);
    vkFreeMemory(device, memory, nullptr);
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
