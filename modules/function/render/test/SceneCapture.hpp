#pragma once
#include <lux/engine/render/resources/lighting/Resource.hpp>
#include <lux/engine/render/resources/instance/Resource.hpp>
#include <lux/engine/render/resources/material/Resource.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>
#include <lux/engine/render/resources/mesh/Resource.hpp>

#include <lux/engine/render/resources/instance/Resource.hpp>
#include <lux/engine/render/resources/lighting/Resource.hpp>

#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>
#include <lux/engine/render/capabilities/RenderPresets.hpp>

#include "Support.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
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

struct CaptureDescriptor;

struct RenderProbeCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using Resources = std::tuple<
        MeshResource, TextureResource, MaterialResource, InstanceResource, LightResource, CaptureDescriptor
    >;
};

// Test configurations compose existing typed Feature inputs; preserve their real cold memory accounting.
namespace lux::render
{
    template <class... Fields> struct ConfigValueTraits<std::tuple<Fields...>>
    {
        static RenderResult<std::size_t> retainedBytes(const std::tuple<Fields...>& value) noexcept
        {
            RenderResult<std::size_t> total{sizeof(value)};
            std::apply(
                [&](const auto&... fields) noexcept
                {
                    const auto add = [&](const auto& field) noexcept
                    {
                        if (!total) return;
                        using Field = std::remove_cvref_t<decltype(field)>;
                        auto bytes = ConfigValueTraits<Field>::retainedBytes(field);
                        if (!bytes) total = lux::cxx::unexpected(bytes.error());
                        else if (*bytes < sizeof(Field) || *bytes - sizeof(Field) > SIZE_MAX - *total)
                            total = lux::cxx::unexpected(RenderError{kInvalidConfig});
                        else *total += *bytes - sizeof(Field);
                    };
                    (add(fields), ...);
                },
                value
            );
            return total;
        }
    };
}

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

// Test-only capability: implementations produce explicit render/readback probes.
// No Graph description is carried in its public Commands or frame protocol.


inline std::vector<std::string>
probePorts(std::initializer_list<std::string_view> images, std::initializer_list<std::string_view> buffers)
{
    std::vector<std::string> names;
    for (auto key : images) names.emplace_back(key);
    for (auto key : buffers) names.emplace_back(key);
    return names;
}

template <class Configuration, class Capability = RenderProbeCapability> struct RenderProbe final
{
    using Build = RenderResult<EffectDefinition> (*)(
        const EffectOutputDesc&, const Configuration&, std::span<const FeatureId>
    ) noexcept;
    CapabilityImplementationId implementation;
    std::vector<FeatureId> features;
    std::vector<std::string> outputs;
    Build build;
};

template <class Configuration, class Capability = RenderProbeCapability>
RenderProbe<Configuration, Capability> registerRenderProbe(
    RenderServer& server, std::string name, std::vector<FeatureId> features, std::vector<std::string> outputs,
    typename RenderProbe<Configuration, Capability>::Build build
)
{
    CapabilityImplementation implementation;
    implementation.implementation = std::move(name);
    implementation.features = features;
    using enum rdesc::ETextureFormat;
    using enum ETargetKind;
    implementation.outputs = {
        {RGBA8_UNORM, 1, OFFSCREEN}, {RGBA8_UNORM, 1, SURFACE},
        {RGBA16_SFLOAT, 1, OFFSCREEN}, {RGBA16_SFLOAT, 1, SURFACE}
    };
    auto capability = server.capabilityId(lux::cxx::make_basic_type_info<Capability>());
    if (!capability && capability.error().type == kRuntimeIdentity)
        capability = server.registerCapability<Capability>();
    const auto id = take(server.addCapabilityImplementation(take(std::move(capability)), implementation));
    return {id, std::move(features), std::move(outputs), build};
}

template <class Configuration, class Capability = RenderProbeCapability>
CreateEffectRequest probeRequest(
    TargetDesc target, const RenderProbe<Configuration, Capability>& probe, Configuration configuration,
    std::initializer_list<std::string_view> outputs
)
{
    CreateEffectRequest request{EffectOutputDesc{target.format, target.samples, target.kind, target.sampled}};
    request.definition = take(probe.build(request.output, configuration, probe.features));
    CHECK(request.definition.outputs.size() == probe.outputs.size());
    std::vector<FeatureExport> selected;
    for (auto name : outputs)
    {
        const auto found = std::ranges::find(probe.outputs, name);
        CHECK(found != probe.outputs.end());
        selected.push_back(request.definition.outputs[found - probe.outputs.begin()]);
    }
    request.definition.outputs = std::move(selected);
    for (auto& feature : request.definition.features) feature.contribution_key = "probe/" + feature.contribution_key;
    request.implementations.push_back({probe.implementation, "probe"});
    return request;
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

struct CaptureCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using Resources = std::tuple<CaptureDescriptor>;
};

inline CapabilityImplementationId registerCaptureProbe(RenderServer& server, FeatureId feature)
{
    CapabilityImplementation implementation;
    implementation.implementation = "test.capture";
    implementation.features = {feature};
    implementation.outputs = {{rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN}};
    const auto capability = take(server.registerCapability<CaptureCapability>());
    return take(server.addCapabilityImplementation(capability, implementation));
}

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
        static const StorageBindingOps bindings{resolve};
        return makeResourceRegistration<CaptureDescriptor, CaptureBuffer, &CaptureBuffer::create>(nullptr, &bindings);
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
        void* object, const VGraphResourceDesc&, const void* argument, StorageSubscription&,
        std::span<ResourceInput> output
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

struct WindowClear final
{
    struct Config final
    {
        rdesc::ETextureFormat format;
    };
    using Ports = WindowClear;
    static constexpr FeatureName name{"test.window.clear"};

    static RenderResult<void> describe(const void* configuration, std::string_view, FeaturePorts& ports) noexcept
    {
        GraphImageDesc image{64, 64, 1, 1, static_cast<const Config*>(configuration)->format};
        image.extent.source = EImageExtentSource::TARGET;
        FeaturePort color{"color", EFeaturePort::OUTPUT, image};
        if (image.format == rdesc::ETextureFormat::RGBA16_SFLOAT)
            color.semantic = "lux.color.linear-hdr.v1";
        ports.resources.push_back(std::move(color));
        return {};
    }

    static RenderResult<void> build(const void* configuration, std::string_view, FeatureBuildContext& context) noexcept
    {
        const auto color = context.resource("color");
        const bool is_depth = static_cast<const Config*>(configuration)->format == rdesc::ETextureFormat::D32_SFLOAT;
        const auto clear = is_depth ? ClearImage{{color, GraphImageRange{2}}, {}, .25f}
                                    : ClearImage{{color}, {.25f, .5f, .75f, 1}};
        auto pass = context.graph.addTransferPass("clear output", TransferWork{{clear}});
        if (!pass) return lux::cxx::unexpected(pass.error());
        return context.publish("color", pass->written.front());
    }
};

struct CaptureFeature
{
    using Config = CaptureConfig;
    static constexpr FeatureName name{"test.deferred.Copy"};
    using Storages = std::tuple<CaptureBuffer>;
    using Commands = std::tuple<>;
    using Ports = CaptureFeature;

    static RenderResult<void> describe(const void* value, std::string_view key, FeaturePorts& ports) noexcept
    {
        GraphImageDesc image{64, 64};
        const auto& config = *static_cast<const CaptureConfig*>(value);
        image.format = config.format;
        image.layers = config.layers;
        image.mips = config.mips;
        image.extent.source = config.target_relative ? EImageExtentSource::TARGET : EImageExtentSource::FIXED;
        ports.resources.push_back(
            {"color",
             EFeaturePort::INPUT,
             config.bytes ? VGraphResourceDesc{GraphBufferDesc{config.bytes}} : VGraphResourceDesc{image}}
        );
        ports.resource_bindings.view.push_back(
            {"pixels",
             EFeaturePort::OUTPUT,
             GraphBufferDesc{std::max(64u * 256, config.bytes)},
             EGraphOrigin::IMPORTED,
             {},
             ResourceRequirement{ResourceName(CaptureDescriptor::descriptor.type.name())}}
        );
        return {};
    }

    static RenderResult<void>
    build(
        const void* value,
        std::string_view key,
        FeatureBuildContext& context
    ) noexcept
    {
        context.require(StorageArgument::make(context.resource("pixels").resource,
            *static_cast<const CaptureConfig*>(value)));
        auto initialized = context.graph.addTransferPass(
            featureKey(FeatureName{"test.deferred.Copy"}, key, "initialize"),
            TransferWork{{FillBuffer{{context.resource("pixels")}, 0}}}
        );
        if (!initialized)
        {
            return lux::cxx::unexpected(initialized.error());
        }
        const auto& config = *static_cast<const CaptureConfig*>(value);
        TransferWork copy;
        if (config.bytes)
        {
            copy.operations.push_back(CopyBuffer{
                {context.resource("color")},
                {initialized->written.front(), GraphBufferRange{0, config.bytes}, EGraphAccess::READ_WRITE}
            });
        }
        else
        {
            const auto aspects = rdesc::textureAspectMask(config.format);
            copy.operations.push_back(CopyImageBuffer{
                {context.resource("color"), GraphImageRange{aspects, config.mip, 1, config.layer, 1}},
                {initialized->written.front(), {}, EGraphAccess::READ_WRITE}
            });
        }
        auto pass =
            context.graph.addTransferPass(featureKey(FeatureName{"test.deferred.Copy"}, key, "copy"), std::move(copy));
        if (!pass)
        {
            return lux::cxx::unexpected(pass.error());
        }
        return context.publish("pixels", pass->written.front());
    }
};


// Fixture-owned correlation across its sequential calls; production Clients own their own counters.
static std::uint64_t nextSceneRequest() noexcept
{
    static std::atomic<std::uint64_t> sequence{1};
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

template <class C>
static typename C::Response
call(ipc::IpcRequester& client, const CommandList& catalog, typename C::Request request, bool success = true)
{
    const auto found = std::ranges::find(catalog.commands, C::name, &CommandInfo::name);
    if (found == catalog.commands.end())
        std::fprintf(stderr, "Missing registered command: %.*s\n", int(C::name.size()), C::name.data());
    CHECK(found != catalog.commands.end());
    const auto request_id = nextSceneRequest();
    ipc::IpcMessage message;
    message.emplace<typename C::Request>(std::move(request));
    CHECK(client.trySend(commandTag(found->id), request_id, message));
    typename C::Response response{};
    const auto deadline = Clock::now() + 30s;
    auto& notifications = ipc::IpcCore::instance();
    for (;;)
    {
        const auto observed = notifications.workEpoch();
        std::uint64_t correlation = 0;
        const auto status = client.tryReceive(correlation, message);
        CHECK(status != ipc::EIpcReceive::CLOSED && Clock::now() < deadline);
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

static void renderFrame(ipc::IpcRequester& client, const CommandList& commands, RenderJobId job)
{
    CHECK(call<RequestRenderJob>(client, commands, {job}).job == job);
}

static RenderJobId startRenderJob(
    ipc::IpcRequester& client,
    const CommandList& commands,
    SceneId scene,
    ViewId view,
    TargetId target,
    EffectId effect
)
{
    const auto job = call<CreateRenderJob>(client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    call<StartRenderJob>(client, commands, {job, target});
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

struct SceneInputRoutes final
{
    ResourceInstanceId instances, lights;
    VRenderScope scope;
    std::vector<CapabilityImplementationInfo> recipes;
};

static SceneInputRoutes installInputs(ipc::IpcRequester& client, const CommandList& commands, VRenderScope scope)
{
    const auto choices = call<ListCapabilities>(client, commands, {});
    SceneInputRoutes routes{{}, {}, scope};
    for (auto name : {kRasterSurfaceCapability, kDeferredLightingCapability})
    {
        const auto found = std::ranges::find_if(
            choices.implementations,
            [name](const auto& value) { return value.capability.type.name() == name; }
        );
        CHECK(found != choices.implementations.end());
        routes.recipes.push_back(*found);
    }
    if (!std::holds_alternative<GlobalRenderScope>(scope))
    {
        routes.instances = call<GetOrCreateResource>(client, commands, {scope, kInstanceResourceName}).resource_id;
        routes.lights = call<GetOrCreateResource>(client, commands, {scope, kLightResourceName}).resource_id;
    }
    return routes;
}

// Professional commands also work without a running Job.
template <class Target, class T>
static AckResponse updateSceneData(
    ipc::IpcRequester& client, const CommandList& commands, Target target, const T& input, bool success = true
)
{
    if constexpr (std::same_as<Target, ViewRequest>)
        return call<UpdateViewCamera>(client, commands, {target, input}, success);
    else if constexpr (std::same_as<T, std::span<const DirectLightUpdate>>)
        return call<LightingCommands::Update>(client, commands, {target, input}, success);
    else
        return call<InstanceCommands::Update>(client, commands, {target, {}, input}, success);
}
