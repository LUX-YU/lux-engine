#pragma once
#include <lux/engine/render/resources/lighting/Resource.hpp>
#include <lux/engine/render/resources/instance/Resource.hpp>
#include <lux/engine/render/resources/material/Resource.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>
#include <lux/engine/render/resources/mesh/Resource.hpp>
#include <lux/engine/render/resources/transform/Resource.hpp>

#include "Support.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/effect/ToneMapping.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/resources/texture/Binding.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/resources/lighting/Binding.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/resources/lighting/Data.hpp>
#include <lux/engine/render/resources/instance/Data.hpp>
#include <lux/engine/render/resources/mesh/Data.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <lux/engine/render/resources/texture/Data.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <map>
#include <tuple>
#include <initializer_list>
#include <thread>

using namespace foundation_test;
namespace ipc = lux::platform::ipc;
namespace rdesc = lux::rdesc;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Test-only observation: installed by the first CaptureBuffer before the first frame.
// The fixture serializes uploads/Attach before frame submission. Installation never races
// a producer; native program preparation can subsequently run on CPU workers.
inline nri::CoreInterface call_capture_original{};
inline bool call_capture_first_instance = false;
inline nri::CoreInterface* call_capture_core = nullptr;
inline std::atomic<std::uint64_t> call_capture_fail_set = 0;
inline std::atomic<std::uint64_t> call_capture_fail_pool = 0;
inline std::atomic<std::uint64_t> call_capture_failed_allocations = 0;
inline std::array<std::atomic<std::uint64_t>, 45> call_capture_counts{};
inline std::atomic<std::uint64_t> call_capture_graphics_pipeline_ns = 0;
inline std::atomic<std::uint64_t> call_capture_compute_pipeline_ns = 0;
inline std::atomic<std::uint64_t> call_capture_placed_buffers = 0;

inline void beginCallCapture(const nri::CoreInterface& input) noexcept
{
    if (call_capture_core || !std::getenv("LUX_DRAW_CALL_COUNTS"))
    {
        return;
    }
    // DeviceSession owns a non-const interface and destroys it before Server::run returns.
    auto& core = const_cast<nri::CoreInterface&>(input);
    call_capture_core = &core;
    call_capture_original = core;
    core.CreateDescriptorPool = [](
        nri::Device& device,
        const nri::DescriptorPoolDesc& descriptorPoolDesc,
        nri::DescriptorPool*& descriptorPool
    ) noexcept -> nri::Result
    {
        const auto attempt = call_capture_counts[0].fetch_add(1, std::memory_order_relaxed) + 1;
        if (call_capture_fail_pool.load(std::memory_order_relaxed) == attempt)
        {
            call_capture_fail_pool.store(0, std::memory_order_relaxed);
            call_capture_failed_allocations.fetch_add(1, std::memory_order_relaxed);
            return nri::Result::OUT_OF_MEMORY;
        }
        return call_capture_original.CreateDescriptorPool(device, descriptorPoolDesc, descriptorPool);
    };
    core.CreatePipelineLayout = [](
        nri::Device& device,
        const nri::PipelineLayoutDesc& pipelineLayoutDesc,
        nri::PipelineLayout*& pipelineLayout
    ) noexcept -> nri::Result
    {
        call_capture_counts[1].fetch_add(1, std::memory_order_relaxed);
        return call_capture_original.CreatePipelineLayout(device, pipelineLayoutDesc, pipelineLayout);
    };
    core.CreateGraphicsPipeline = [](
        nri::Device& device,
        const nri::GraphicsPipelineDesc& graphicsPipelineDesc,
        nri::Pipeline*& pipeline
    ) noexcept -> nri::Result
    {
        call_capture_counts[2].fetch_add(1, std::memory_order_relaxed);
        const auto begin = Clock::now();
        const auto result = call_capture_original.CreateGraphicsPipeline(device, graphicsPipelineDesc, pipeline);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
        call_capture_graphics_pipeline_ns.fetch_add(elapsed, std::memory_order_relaxed);
        return result;
    };
    core.CreateComputePipeline = [](
        nri::Device& device, const nri::ComputePipelineDesc& description, nri::Pipeline*& pipeline
    ) noexcept -> nri::Result
    {
        call_capture_counts[40].fetch_add(1, std::memory_order_relaxed);
        const auto begin = Clock::now();
        const auto result = call_capture_original.CreateComputePipeline(device, description, pipeline);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
        call_capture_compute_pipeline_ns.fetch_add(elapsed, std::memory_order_relaxed);
        return result;
    };
    core.DestroyPipeline = [](nri::Pipeline* pipeline) noexcept
    {
        call_capture_counts[41].fetch_add(pipeline != nullptr, std::memory_order_relaxed);
        call_capture_original.DestroyPipeline(pipeline);
    };
    core.DestroyPipelineLayout = [](nri::PipelineLayout* layout) noexcept
    {
        call_capture_counts[42].fetch_add(layout != nullptr, std::memory_order_relaxed);
        call_capture_original.DestroyPipelineLayout(layout);
    };
    core.CreateBufferView = [](
        const nri::BufferViewDesc& bufferViewDesc,
        nri::Descriptor*& bufferView
    ) noexcept -> nri::Result
    {
        call_capture_counts[3].fetch_add(1, std::memory_order_relaxed);
        return call_capture_original.CreateBufferView(bufferViewDesc, bufferView);
    };
    core.CreatePlacedBuffer = [](
        nri::Device& device, nri::Memory* memory, std::uint64_t offset,
        const nri::BufferDesc& description, nri::Buffer*& buffer
    ) noexcept
    {
        const auto result = call_capture_original.CreatePlacedBuffer(device, memory, offset, description, buffer);
        if (result == nri::Result::SUCCESS) call_capture_placed_buffers.fetch_add(1, std::memory_order_relaxed);
        return result;
    };
    core.AllocateDescriptorSets = [](
        nri::DescriptorPool& descriptorPool,
        const nri::PipelineLayout& pipelineLayout,
        uint32_t setIndex,
        nri::DescriptorSet** descriptorSets,
        uint32_t instanceNum,
        uint32_t variableDescriptorNum
    ) noexcept -> nri::Result
    {
        const auto attempt = call_capture_counts[4].fetch_add(1, std::memory_order_relaxed) + 1;
        if (call_capture_fail_set.load(std::memory_order_relaxed) == attempt)
        {
            call_capture_fail_set.store(0, std::memory_order_relaxed);
            call_capture_failed_allocations.fetch_add(1, std::memory_order_relaxed);
            return nri::Result::OUT_OF_MEMORY;
        }
        const auto result = call_capture_original.AllocateDescriptorSets(
            descriptorPool, pipelineLayout, setIndex, descriptorSets, instanceNum, variableDescriptorNum
        );
        if (result == nri::Result::SUCCESS)
        {
            call_capture_counts[34].fetch_add(instanceNum, std::memory_order_relaxed);
        }
        return result;
    };
    core.UpdateDescriptorRanges = [](
        const nri::UpdateDescriptorRangeDesc* updateDescriptorRangeDescs,
        uint32_t updateDescriptorRangeDescNum
    ) noexcept -> void
    {
        call_capture_counts[5].fetch_add(1, std::memory_order_relaxed);
        call_capture_counts[35].fetch_add(updateDescriptorRangeDescNum, std::memory_order_relaxed);
        call_capture_original.UpdateDescriptorRanges(updateDescriptorRangeDescs, updateDescriptorRangeDescNum);
    };
    core.BeginCommandBuffer = [](
        nri::CommandBuffer& commandBuffer,
        const nri::DescriptorPool* descriptorPool
    ) noexcept -> nri::Result
    {
        call_capture_counts[6].fetch_add(1, std::memory_order_relaxed);
        return call_capture_original.BeginCommandBuffer(commandBuffer, descriptorPool);
    };
    core.CmdSetDescriptorPool = [](
        nri::CommandBuffer& commandBuffer,
        const nri::DescriptorPool& descriptorPool
    ) noexcept -> void
    {
        call_capture_counts[7].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetDescriptorPool(commandBuffer, descriptorPool);
    };
    core.CmdSetPipelineLayout = [](
        nri::CommandBuffer& commandBuffer,
        nri::BindPoint bindPoint,
        const nri::PipelineLayout& pipelineLayout
    ) noexcept -> void
    {
        call_capture_counts[8].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetPipelineLayout(commandBuffer, bindPoint, pipelineLayout);
    };
    core.CmdSetDescriptorSet = [](
        nri::CommandBuffer& commandBuffer,
        const nri::SetDescriptorSetDesc& setDescriptorSetDesc
    ) noexcept -> void
    {
        call_capture_counts[9].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetDescriptorSet(commandBuffer, setDescriptorSetDesc);
    };
    core.CmdSetRootConstants = [](
        nri::CommandBuffer& commandBuffer,
        const nri::SetRootConstantsDesc& setRootConstantsDesc
    ) noexcept -> void
    {
        call_capture_counts[10].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetRootConstants(commandBuffer, setRootConstantsDesc);
    };
    core.CmdSetPipeline = [](
        nri::CommandBuffer& commandBuffer,
        const nri::Pipeline& pipeline
    ) noexcept -> void
    {
        call_capture_counts[11].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetPipeline(commandBuffer, pipeline);
    };
    core.CmdBarrier = [](
        nri::CommandBuffer& commandBuffer,
        const nri::BarrierDesc& barrierDesc
    ) noexcept -> void
    {
        call_capture_counts[12].fetch_add(1, std::memory_order_relaxed);
        call_capture_counts[25].fetch_add(barrierDesc.textureNum, std::memory_order_relaxed);
        call_capture_counts[26].fetch_add(barrierDesc.bufferNum, std::memory_order_relaxed);
        call_capture_counts[27].fetch_add(barrierDesc.globalNum, std::memory_order_relaxed);
        call_capture_original.CmdBarrier(commandBuffer, barrierDesc);
    };
    core.CmdSetIndexBuffer = [](
        nri::CommandBuffer& commandBuffer,
        const nri::Buffer& buffer,
        uint64_t offset,
        nri::IndexType indexType
    ) noexcept -> void
    {
        call_capture_counts[13].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetIndexBuffer(commandBuffer, buffer, offset, indexType);
    };
    core.CmdSetVertexBuffers = [](
        nri::CommandBuffer& commandBuffer,
        uint32_t baseSlot,
        const nri::VertexBufferDesc* vertexBufferDescs,
        uint32_t vertexBufferNum
    ) noexcept -> void
    {
        call_capture_counts[14].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetVertexBuffers(commandBuffer, baseSlot, vertexBufferDescs, vertexBufferNum);
    };
    core.CmdSetScissors = [](
        nri::CommandBuffer& commandBuffer,
        const nri::Rect* rects,
        uint32_t rectNum
    ) noexcept -> void
    {
        call_capture_counts[15].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetScissors(commandBuffer, rects, rectNum);
    };
    core.CmdSetDepthBias = [](
        nri::CommandBuffer& commandBuffer,
        const nri::DepthBiasDesc& depthBiasDesc
    ) noexcept -> void
    {
        call_capture_counts[16].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdSetDepthBias(commandBuffer, depthBiasDesc);
    };
    core.CmdBeginRendering = [](
        nri::CommandBuffer& commandBuffer,
        const nri::RenderingDesc& renderingDesc
    ) noexcept -> void
    {
        call_capture_counts[17].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdBeginRendering(commandBuffer, renderingDesc);
    };
    core.CmdDraw = [](
        nri::CommandBuffer& commandBuffer,
        const nri::DrawDesc& drawDesc
    ) noexcept -> void
    {
        call_capture_counts[18].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdDraw(commandBuffer, drawDesc);
    };
    core.CmdDrawIndexed = [](
        nri::CommandBuffer& commandBuffer,
        const nri::DrawIndexedDesc& drawIndexedDesc
    ) noexcept -> void
    {
        call_capture_counts[19].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdDrawIndexed(commandBuffer, drawIndexedDesc);
    };
    core.CmdDrawIndexedIndirect = [](
        nri::CommandBuffer& commandBuffer,
        const nri::Buffer& buffer,
        uint64_t offset,
        uint32_t drawNum,
        uint32_t stride,
        const nri::Buffer* countBuffer,
        uint64_t countBufferOffset
    ) noexcept -> void
    {
        call_capture_counts[20].fetch_add(1, std::memory_order_relaxed);
        call_capture_counts[33].fetch_add(drawNum, std::memory_order_relaxed);
        call_capture_counts[36].fetch_add(drawNum > 1, std::memory_order_relaxed);
        call_capture_counts[39].fetch_add(countBuffer != nullptr, std::memory_order_relaxed);
        const auto maximum = [](std::atomic<std::uint64_t>& value, std::uint64_t sample) noexcept
        {
            auto previous = value.load(std::memory_order_relaxed);
            while (sample > previous && !value.compare_exchange_weak(previous, sample, std::memory_order_relaxed)) {}
        };
        maximum(call_capture_counts[37], drawNum);
        maximum(call_capture_counts[38], stride);
        call_capture_original.CmdDrawIndexedIndirect(
            commandBuffer, buffer, offset, drawNum, stride, countBuffer, countBufferOffset
        );
    };
    core.CmdEndRendering = [](
        nri::CommandBuffer& commandBuffer
    ) noexcept -> void
    {
        call_capture_counts[21].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdEndRendering(commandBuffer);
    };
    core.CmdDispatch = [](
        nri::CommandBuffer& commandBuffer,
        const nri::DispatchDesc& dispatchDesc
    ) noexcept -> void
    {
        call_capture_counts[22].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdDispatch(commandBuffer, dispatchDesc);
    };
    core.CmdDispatchIndirect = [](
        nri::CommandBuffer& commandBuffer,
        const nri::Buffer& buffer,
        uint64_t offset
    ) noexcept -> void
    {
        call_capture_counts[23].fetch_add(1, std::memory_order_relaxed);
        call_capture_original.CmdDispatchIndirect(commandBuffer, buffer, offset);
    };
    core.CmdCopyBuffer = [](
        nri::CommandBuffer& command, nri::Buffer& destination, std::uint64_t destination_offset,
        const nri::Buffer& source, std::uint64_t source_offset, std::uint64_t bytes
    ) noexcept
    {
        call_capture_counts[43].fetch_add(1, std::memory_order_relaxed);
        call_capture_counts[44].fetch_add(bytes, std::memory_order_relaxed);
        call_capture_original.CmdCopyBuffer(command, destination, destination_offset, source, source_offset, bytes);
    };
    core.QueueSubmit = [](
        nri::Queue& queue,
        const nri::QueueSubmitDesc& queueSubmitDesc
    ) noexcept -> nri::Result
    {
        call_capture_counts[24].fetch_add(1, std::memory_order_relaxed);
        const auto result = call_capture_original.QueueSubmit(queue, queueSubmitDesc);
        call_capture_counts[result == nri::Result::SUCCESS ? 28 : 29].fetch_add(1, std::memory_order_relaxed);
        if (result == nri::Result::SUCCESS)
        {
            call_capture_counts[30].fetch_add(queueSubmitDesc.waitFenceNum, std::memory_order_relaxed);
            call_capture_counts[31].fetch_add(queueSubmitDesc.signalFenceNum, std::memory_order_relaxed);
            call_capture_counts[32].fetch_add(queueSubmitDesc.commandBufferNum, std::memory_order_relaxed);
        }
        return result;
    };
}

inline void writeCallCounts(const char* path) noexcept
{
    auto* output = std::fopen(path, "w");
    CHECK(output);
    std::fputs("{\n", output);
    std::fprintf(output, "  \"CmdCopyBuffer\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[43].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"copied_buffer_bytes\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[44].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CreateComputePipeline\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[40].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"compute_pipeline_native_ns\": %llu,\n",
        static_cast<unsigned long long>(call_capture_compute_pipeline_ns.load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"DestroyPipeline\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[41].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"DestroyPipelineLayout\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[42].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CreateDescriptorPool\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[0].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CreatePipelineLayout\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[1].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CreateGraphicsPipeline\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[2].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"graphics_pipeline_native_ns\": %llu,\n",
        static_cast<unsigned long long>(call_capture_graphics_pipeline_ns.load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CreateBufferView\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[3].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"AllocateDescriptorSets\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[4].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"UpdateDescriptorRanges\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[5].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"BeginCommandBuffer\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[6].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetDescriptorPool\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[7].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetPipelineLayout\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[8].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetDescriptorSet\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[9].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetRootConstants\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[10].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetPipeline\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[11].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdBarrier\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[12].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetIndexBuffer\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[13].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetVertexBuffers\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[14].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetScissors\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[15].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdSetDepthBias\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[16].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdBeginRendering\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[17].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdDraw\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[18].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdDrawIndexed\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[19].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdDrawIndexedIndirect\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[20].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdEndRendering\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[21].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdDispatch\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[22].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"CmdDispatchIndirect\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[23].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"QueueSubmit\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[24].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"image_barriers\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[25].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"buffer_barriers\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[26].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"global_barriers\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[27].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"submit_success\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[28].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"submit_failure\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[29].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"waits\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[30].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"signals\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[31].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"submitted_command_buffers\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[32].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"indirect_record_capacity\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[33].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"multi_draw_calls\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[36].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"max_indirect_records_per_call\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[37].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"max_indirect_stride\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[38].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"count_buffer_calls\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[39].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"allocated_sets\": %llu,\n",
        static_cast<unsigned long long>(call_capture_counts[34].load(std::memory_order_relaxed)));
    std::fprintf(output, "  \"updated_ranges\": %llu\n",
        static_cast<unsigned long long>(call_capture_counts[35].load(std::memory_order_relaxed)));
    std::fputs("}\n", output);
    std::fclose(output);
}

inline void endCallCapture() noexcept
{
    if (call_capture_core)
    {
        // Called after run() joins: DeviceSession has already been destroyed. Never write back
        // through this identity. Function targets and counters are test-static and outlive it.
        call_capture_core = nullptr;
        writeCallCounts(std::getenv("LUX_DRAW_CALL_COUNTS"));
    }
}

// Test extension: ordinary graph copies, mapped only after Detach and actual GPU cleanup.
// No production Readback command, implicit Draw, Native accessor on Client, or timing-based completion.
struct CapturedPixels
{
    std::vector<std::vector<std::byte>> frames;
    std::uint32_t pitch = 0;
    std::atomic<unsigned> ready = 0;
};

struct CaptureConfig
{
    CapturedPixels* result = nullptr;
    rdesc::ETextureFormat format = rdesc::ETextureFormat::RGBA8_UNORM;
    std::uint32_t layers = 1, layer = 0, bytes = 0;
    bool target_relative = true;
    std::uint32_t mips = 1, mip = 0;
    bool operator==(const CaptureConfig&) const noexcept = default;
};

struct CaptureDescriptor
{
    using Config = void;
    using Ops = void;
    static inline const ResourceDefinition descriptor{
        ResourceNameView{"test.deferred.Capture"}, 0, 0, lux::cxx::make_basic_type_info<CaptureConfig>(),
        "View-owned terminal copy collection.", EResourceDomain::VIEW
    };
};

class CaptureBuffer
{
    struct Allocation
    {
        CaptureConfig config;
        std::vector<nri::Buffer*> buffers;
        std::uint32_t pitch;
        std::uint64_t bytes;
    };

public:
    using Descriptor = CaptureDescriptor;

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<CaptureDescriptor, CaptureBuffer, &CaptureBuffer::create>(nullptr, nullptr);
    }

    explicit CaptureBuffer(ResourceCreateContext& context) noexcept
        : device_(context.device), frame_count_(context.frame_count) {}

    ~CaptureBuffer() noexcept
    {
        auto* device = &device_;
        for (auto& allocation : allocations_)
        {
            device_.deferRelease([device, allocation = std::move(allocation)]() noexcept
            {
                auto* result = allocation.config.result;
                result->pitch = allocation.pitch;
                for (auto* buffer : allocation.buffers)
                {
                    const auto* mapped = static_cast<const std::byte*>(
                        device->nriCore().MapBuffer(*buffer, 0, allocation.bytes));
                    CHECK(mapped);
                    CHECK(device->invalidateMappedRange(*buffer, 0, allocation.bytes) == nri::Result::SUCCESS);
                    result->frames.emplace_back(mapped, mapped + allocation.bytes);
                    device->nriCore().UnmapBuffer(*buffer);
                    device->nriCore().DestroyBuffer(buffer);
                }
                result->ready.fetch_add(1, std::memory_order_release);
            });
        }
    }

    static RenderResult<std::unique_ptr<CaptureBuffer>>
    create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        beginCallCapture(context.device.nriCore());
        call_capture_first_instance = context.device.enabledFeatures().indirect_first_instance;
        return std::make_unique<CaptureBuffer>(context);
    }

    static RenderResult<void> resolve(
        void* object, const VGraphResourceDesc&, const void* argument, std::span<ResourceInput> output
    ) noexcept
    {
        auto& value = *static_cast<CaptureBuffer*>(object);
        const auto& config = *static_cast<const CaptureConfig*>(argument);
        if (!config.result || output.size() != value.frame_count_) return lux::cxx::unexpected(RenderError{kStorageContract});
        auto found = std::ranges::find(value.allocations_, config, &Allocation::config);
        if (found == value.allocations_.end())
        {
            const auto alignment = value.device_.description().memoryAlignment.uploadBufferTextureRow;
            const auto pitch = (256 + alignment - 1) / alignment * alignment;
            const auto bytes = std::max<std::uint64_t>(pitch * 64u, config.bytes);
            Allocation allocation{config, std::vector<nri::Buffer*>(value.frame_count_), pitch, bytes};
            for (auto*& buffer : allocation.buffers)
            {
                CHECK(value.device_.nriCore().CreatePlacedBuffer(value.device_.device(), NriHostReadbackHeap,
                    {bytes, 0, nri::BufferUsageBits::NONE}, buffer) == nri::Result::SUCCESS);
                auto* mapped = value.device_.nriCore().MapBuffer(*buffer, 0, bytes);
                CHECK(mapped);
                std::memset(mapped, 0, bytes);
                CHECK(value.device_.flushMappedRange(*buffer, 0, bytes) == nri::Result::SUCCESS);
                value.device_.nriCore().UnmapBuffer(*buffer);
            }
            value.allocations_.push_back(std::move(allocation));
            found = std::prev(value.allocations_.end());
        }
        for (std::size_t i = 0; i < output.size(); ++i) output[i] = {found->buffers[i], 0, found->bytes};
        return {};
    }

private:
    DeviceSession& device_;
    std::uint32_t frame_count_;
    std::vector<Allocation> allocations_;
};

struct GraphCaptureDescriptor final
{
    using Config = void;
    using Ops = void;
    static inline const ResourceDefinition descriptor{
        ResourceNameView{"test.graph.capture"}, 0, 0, {}, "View-owned EffectGraph readback.", EResourceDomain::VIEW
    };
};

class GraphCaptureBuffer final
{
public:
    using Descriptor = GraphCaptureDescriptor;
    static constexpr std::uint32_t image_bytes = 64u * 256;

    static ResourceRegistration factory() noexcept
    {
        static const StorageBindingOps bindings{
            .describeGraphImport = describe,
            .resolveGraphImport = resolve
        };
        return makeResourceRegistration<Descriptor, GraphCaptureBuffer, &GraphCaptureBuffer::create>(nullptr, &bindings);
    }

    static ResourceInstanceId
    bind(ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId device,
        ViewRequest view, const CaptureConfig& config, const CaptureConfig& extra = {});

    static ResourceInstanceId
    bind(ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId device,
        ViewRequest view, std::span<const CaptureConfig> configurations);

    GraphCaptureBuffer(std::unique_ptr<CaptureBuffer> storage, std::span<const CaptureConfig> configurations) noexcept
        : storage_(std::move(storage)), config_(configurations.begin(), configurations.end())
    {
    }

private:
    static RenderResult<std::unique_ptr<GraphCaptureBuffer>>
    create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
    {
        const auto* config = creation_input_.load();
        if (!config) return lux::cxx::unexpected(RenderError{kInvalidConfig});
        auto storage = CaptureBuffer::create(context, id);
        if (!storage) return lux::cxx::unexpected(storage.error());
        return std::make_unique<GraphCaptureBuffer>(std::move(*storage), *config);
    }

    static RenderResult<StorageGraphImport> describe(const void* object, std::uint32_t projection) noexcept
    {
        const auto& capture = *static_cast<const GraphCaptureBuffer*>(object);
        if (projection >= capture.config_.size() || !capture.config_[projection].result)
            return lux::cxx::unexpected(RenderError{kStorageContract});
        const auto bytes = std::max(image_bytes, capture.config_[projection].bytes);
        return StorageGraphImport::bufferOutput(bytes, EGraphUse::TRANSFER_DESTINATION);
    }

    static RenderResult<void> resolve(
        void* object, std::uint32_t projection, const VGraphResourceDesc& description,
        std::span<ResourceInput> output
    ) noexcept
    {
        auto& capture = *static_cast<GraphCaptureBuffer*>(object);
        if (projection >= capture.config_.size() || !capture.config_[projection].result)
            return lux::cxx::unexpected(RenderError{kStorageContract});
        return CaptureBuffer::resolve(capture.storage_.get(), description, &capture.config_[projection], output);
    }

    static inline std::atomic<const std::span<const CaptureConfig>*> creation_input_{};
    std::unique_ptr<CaptureBuffer> storage_;
    std::vector<CaptureConfig> config_;
};

struct ColorCapture final
{
    static constexpr std::string_view name = "test.graph.capture_color";
    static constexpr auto color = effectPortSemantic("color");
    static constexpr auto pixels = effectPortSemantic("pixels");

    static CapabilityDefinition definition()
    {
        return {
            .identity = {lux::graph::nodeTypeId(name), std::string{name}, 1},
            .ports = {
                {color, "color", {std::string{ToneMapping::display_type}, 1}, lux::graph::EPinDirection::INPUT, 1, true},
                {pixels, "pixels", {"test.graph.pixels", 1}, lux::graph::EPinDirection::OUTPUT}
            }
        };
    }

    static void registerWith(RenderServer& server)
    {
        take(server.registerResource<GraphCaptureBuffer>());
        const auto capability = definition();
        CHECK(server.registerEffectCapability(capability));
        EffectImplementation implementation;
        implementation.capability = capability.identity.id;
        implementation.identity = {std::string{name}, 1};
        implementation.supported_outputs = {pixels};
        implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept
            -> RenderResult<void> { return {}; };
        implementation.build = build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }

private:
    static RenderResult<void> build(EffectBuildContext& context, std::string_view) noexcept
    {
        const auto input = context.inputs(color);
        CHECK(input.size() == 1 && input.front().size() == 1);
        const auto readback = context.importFinalBuffer(
            name,
            {ResourceName{GraphCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{GraphCaptureBuffer::image_bytes}, EGraphOrigin::IMPORTED, {},
                EGraphUse::TRANSFER_DESTINATION}
        );
        if (!readback) return lux::cxx::unexpected(readback.error());
        const auto copy = take(context.builder().addTransferPass(
            "copy color pixels", TransferWork{{CopyImageBuffer{
                {input.front().front().version}, {*readback}, 256, GraphCaptureBuffer::image_bytes,
                ImageCopyRegion{0, 0, 64, 64}
            }}}
        ));
        return context.publish(pixels, {{pixels, copy.written.front()}});
    }
};

// Fixture-owned correlation across its sequential calls; production Clients own their own counters.
static std::uint64_t nextSceneRequest() noexcept
{
    static std::atomic<std::uint64_t> sequence{1};
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

template <class C>
static std::uint64_t
sendSceneRequest(ipc::IpcRequester& client, const CommandList& catalog, typename C::Request request)
{
    const auto found = std::ranges::find(catalog.commands, C::name, &CommandInfo::name);
    if (found == catalog.commands.end())
        std::fprintf(stderr, "Missing registered command: %.*s\n", int(C::name.size()), C::name.data());
    CHECK(found != catalog.commands.end());
    const auto request_id = nextSceneRequest();
    ipc::IpcMessage message;
    message.emplace<typename C::Request>(std::move(request));
    CHECK(client.trySend(commandTag(found->id), request_id, message));
    return request_id;
}

template <class C>
static typename C::Response receiveSceneReply(ipc::IpcRequester& client, std::uint64_t request_id, bool success = true)
{
    ipc::IpcMessage message;
    typename C::Response response{};
    const auto deadline = Clock::now() + 30s;
    auto& notifications = ipc::IpcCore::instance();
    for (;;)
    {
        const auto observed = notifications.workEpoch();
        std::uint64_t correlation = 0;
        const auto status = client.tryReceive(correlation, message);
        const bool closed = status == ipc::EIpcReceive::CLOSED;
        if (closed || Clock::now() >= deadline)
        {
            std::fflush(stdout);
            std::fprintf(stderr, "IPC request %llu (%.*s) %s, receive status %u, correlation %llu\n",
                static_cast<unsigned long long>(request_id), static_cast<int>(C::name.size()), C::name.data(),
                closed ? "closed" : "timed out", static_cast<unsigned>(status),
                static_cast<unsigned long long>(correlation));
            CHECK(false);
        }
        if (status != ipc::EIpcReceive::RECEIVED)
        {
            notifications.waitUntil(observed, deadline);
            continue;
        }
        // Frames have one terminal, and structural controls are ordinary discrete requests.
        CHECK(correlation == request_id);
        response = std::move(message.get<typename C::Response>());
        break;
    }
    if (success && response.error.type)
    {
        std::fprintf(
            stderr,
            "%.*s error=%llu args=%llu,%llu,%llu\n",
            int(C::name.size()),
            C::name.data(),
            response.error.type,
            response.error.args[0],
            response.error.args[1],
            response.error.args[2]
        );
        CHECK(false);
    }
    return response;
}

template <class C>
static typename C::Response
call(ipc::IpcRequester& client, const CommandList& catalog, typename C::Request request, bool success = true)
{
    return receiveSceneReply<C>(client, sendSceneRequest<C>(client, catalog, std::move(request)), success);
}

inline MaterialProgramId registerMaterialProgram(
    ipc::IpcRequester& client, const CommandList& commands, ResourceInstanceId resource, rdesc::MaterialProgramSet program
)
{
    return call<MaterialCommands::RegisterProgram>(client, commands, {resource, std::move(program)}).program;
}

inline MaterialId createMaterialInstance(
    ipc::IpcRequester& client, const CommandList& commands, MaterialProgramId program,
    MaterialParameters parameters = {}, std::vector<TextureId> textures = {}
)
{
    return call<MaterialCommands::CreateInstance>(
        client, commands, {program, std::move(parameters), std::move(textures)}
    ).material;
}

static ResourceInstanceId sceneTransformStorage(
    ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId device, SceneId scene
)
{
    return call<GetOrCreateResource>(client, commands, {device, scene, kTransformResourceName}).resource_id;
}

static TransformId createTransform(
    ipc::IpcRequester& client,
    const CommandList& commands,
    ResourceInstanceId resource,
    const TransformMatrix& matrix = kIdentityTransform
)
{
    return call<TransformCommands::Create>(client, commands, {resource, std::span{&matrix, 1}}).transforms.front();
}

static std::vector<TransformId> bindTransforms(
    ipc::IpcRequester& client,
    const CommandList& commands,
    ResourceInstanceId resource,
    std::span<MeshInstanceData> instances,
    std::span<const TransformMatrix> matrices
)
{
    CHECK(instances.size() == matrices.size());
    auto transforms = call<TransformCommands::Create>(client, commands, {resource, matrices}).transforms;
    CHECK(transforms.size() == instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i) instances[i].transform_id = transforms[i];
    return transforms;
}

inline ResourceInstanceId GraphCaptureBuffer::bind(
    ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId device,
    ViewRequest view, const CaptureConfig& config, const CaptureConfig& extra
)
{
    const std::array configurations{config, extra};
    return bind(client, commands, device, view, std::span{configurations}.first(extra.result ? 2 : 1));
}

inline ResourceInstanceId GraphCaptureBuffer::bind(
    ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId device,
    ViewRequest view, std::span<const CaptureConfig> configurations
)
{
    // Only this synchronous fixture call supplies the resource's creation input.
    CHECK(!configurations.empty() && !creation_input_.exchange(&configurations));
    const auto resource = call<GetOrCreateResource>(client, commands, {device, view, Descriptor::descriptor.type});
    creation_input_.store(nullptr);
    return resource.resource_id;
}

static void renderFrame(ipc::IpcRequester& client, const CommandList& commands, RenderJobId job)
{
    CHECK(call<RequestRenderJob>(client, commands, {job}).job == job);
}

static EffectId createPreparedEffect(
    ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId device, CreateEffectRequest request
)
{
    const auto effect = call<CreateEffect>(client, commands, std::move(request)).effect;
    call<PrepareEffect>(client, commands, {effect, device});
    return effect;
}

static RenderJobId startRenderJob(
    ipc::IpcRequester& client,
    const CommandList& commands,
    DeviceSessionId session,
    SceneId scene,
    ViewId view,
    TargetId target,
    EffectId effect,
    JobBinding binding = {}
)
{
    const auto job = call<CreateRenderJob>(client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    call<StartRenderJob>(client, commands, {session, job, target, std::move(binding)});
    return job;
}

static void stopAndRemoveJob(ipc::IpcRequester& client, const CommandList& commands, RenderJobId job)
{
    call<StopRenderJob>(client, commands, job);
    const auto deadline = Clock::now() + 20s;
    for (;;)
    {
        const auto response = call<RemoveRenderJob>(client, commands, job, false);
        if (!response.error.type)
        {
            return;
        }
        CHECK(response.error.type == kJobBusy && Clock::now() < deadline);
        std::this_thread::yield();
    }
}

// Bootstrap and discovery stay on the Server owner through the existing IPC protocol.
static CommandList queryCommands(ipc::IpcRequester& client)
{
    CommandList bootstrap;
    bootstrap.commands.push_back({std::string(QueryCommandIds::name), QueryCommandIds::id});
    const std::array names{CommandName{ListCommands::name}};
    const auto ids = call<QueryCommandIds>(client, bootstrap, names);
    bootstrap.commands.push_back({std::string(ListCommands::name), ids.ids.front()});
    return call<ListCommands>(client, bootstrap, {});
}

// Professional commands also work without a running Job.
template <class T>
static AckResponse updateSceneData(
    ipc::IpcRequester& client, const CommandList& commands, ResourceInstanceId target, const T& input, bool success = true
)
{
    if constexpr (std::same_as<T, std::span<const DirectLightUpdate>>)
        return call<LightingCommands::Update>(client, commands, {target, input}, success);
    else
        return call<InstanceCommands::Update>(client, commands, {target, input}, success);
}
