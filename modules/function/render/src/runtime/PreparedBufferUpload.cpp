#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/graph/execute/PreparedBufferUpload.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <algorithm>
#include <bit>

namespace lux::render::detail
{
    RenderResult<bool> PreparedUploadWindow::stage(
        nri::Buffer& buffer, std::uint32_t slot, std::uint64_t generation, PreparedBufferInput& input
    ) noexcept
    {
        const auto offset = input.staged_bytes;
        const auto bytes = std::span<const std::byte>{input.bytes}.subspan(offset);
        const auto first = uploader_->stagedFrameCopyCount();
        auto staged = uploader_->stageBufferPrefix({&buffer, offset, bytes});
        if (!staged) return cxx::unexpected(staged.error());
        if (!*staged) return false;
        input.staged_bytes += *staged;
        copies_.push_back({
            slot, generation, {first, uploader_->stagedFrameCopyCount() - first},
            offset, input.staged_bytes, input.size, input.revision
        });
        return input.staged_bytes == input.size;
    }

    RenderResult<bool> PreparedUploadWindow::stageFrame(
        FrameResources& frame, const compiled::RenderGraph& graph, std::span<PreparedBufferInput> inputs
    ) noexcept
    {
        auto& replicas = frame.native.prepared_buffers;
        if (inputs.size() != replicas.size()) return cxx::unexpected(RenderError{kFrameContract});
        for (std::size_t i = 0; i < replicas.size(); ++i)
        {
            const auto& contract = graph.cpuProgram().buffers[i];
            if (!contract.used) continue;
            if (contract.upload && !uploader_) return cxx::unexpected(RenderError{kDeviceUnsupported});
            auto& replica = replicas[i];
            auto& input = inputs[i];
            const auto size = input.size;
            if (input.source == PreparedBufferInput::ESource::RESIDENT)
            {
                const bool same_allocation = replica.generation == input.resident_generation;
                if (!same_allocation || !replica.contains(input.revision, size))
                    return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
                frame.bindResource(graph, device_, contract.resource, {replica.buffer, 0, size});
                continue;
            }
            if (size > replica.capacity)
            {
                if (replica.generation == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
                const auto capacity = size > std::bit_floor(UINT64_MAX) ?
                    contract.maximum_bytes : std::min(std::bit_ceil(size), contract.maximum_bytes);
                const auto description = graphBufferDescription(device_, graph.resources()[contract.resource], capacity);
                if (!description) return cxx::unexpected(description.error());
                nri::Buffer* candidate = nullptr;
                const auto result =
                    device_.nriCore().CreatePlacedBuffer(device_.device(), NriDeviceHeap, *description, candidate);
                if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
                frame.bindResource(graph, device_, contract.resource, {candidate, 0, size});
                releaseAfterGpu(device_, replica.buffer);
                replica.buffer = candidate;
                ++replica.generation;
                replica.capacity = capacity;
                replica.accepted = {};
            }
            if (!contract.upload)
            {
                frame.bindResource(graph, device_, contract.resource, {replica.buffer, 0, size});
                continue;
            }
            if (input.staged_bytes == size) continue;
            auto complete = stage(*replica.buffer, static_cast<std::uint32_t>(i), replica.generation, input);
            if (!complete || !*complete) return complete;
            frame.bindResource(graph, device_, contract.resource, {replica.buffer, 0, size});
        }
        return true;
    }
} // namespace lux::render::detail
