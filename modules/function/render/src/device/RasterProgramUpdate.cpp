#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/RasterProgramUpdate.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <algorithm>

namespace lux::render::detail
{
    RasterProgramBuildData::Stream* RasterProgramBuildData::find(DrawStreamSlot slot) noexcept
    {
        const auto stream = std::ranges::find(streams, slot.index, &Stream::pass);
        return stream == streams.end() ? nullptr : &*stream;
    }

    bool RasterProgramBuildData::canRead(const Stream& stream, ResourceInstanceId id) const noexcept
    {
        if (id.isNull()) return false;
        if (std::ranges::find(stream.sources, id) != stream.sources.end()) return true;
        return std::ranges::find(snapshots, id, &Snapshot::owner) != snapshots.end();
    }

    RasterProgramUpdate::~RasterProgramUpdate() noexcept
    {
        for (const auto& stream : streams)
        {
            if (!stream.candidate) continue;
            const auto& programs = stream.candidate->programs;
            const auto identity = [&](std::size_t i) noexcept { return programs[i].cache_id; };
            device.programs().release(programs.size(), identity, {});
        }
    }

    exec::task<RenderResult<void>> RasterProgramUpdate::compile(execution::CpuScheduler cpu) noexcept
    {
        for (auto& stream : streams)
        {
            for (auto& [link, source] : stream.programs)
            {
                if (stop.stop_requested()) co_return cxx::unexpected(RenderError{kCommandCancelled});
                auto ready = co_await device.programs().prepareSelectedProgram(
                    std::move(link.input), cpu, stop.get_token()
                );
                if (!ready) co_return cxx::unexpected(ready.error());
                if (!source.tryPublish(registry, device, ready->id))
                    pending_publications.push_back({source, ready->id});
                stream.candidate->programs.push_back(std::move(link).finish(std::move(*ready)));
            }
        }
        co_return RenderResult<void>{};
    }

    bool RasterProgramUpdate::publishReturnedUses() noexcept
    {
        std::erase_if(pending_publications, [&](const Publication& pending) noexcept
        {
            return pending.source.tryPublish(registry, device, pending.program);
        });
        return pending_publications.empty();
    }
} // namespace lux::render::detail

namespace lux::render
{
    RenderResult<void>
    RasterProgramBuildContext::setPrograms(
        DrawStreamSlot slot,
        std::vector<rdesc::ShaderProgramArtifact> shaders,
        std::span<const RuntimeRasterProgram> programs
    ) noexcept
    {
        auto* stream = data_.find(slot);
        if (!stream) return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
        if (stream->programs || stream->reuse)
            return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {slot.index}});
        if (programs.size() >= kGraphInvalidIndex || shaders.size() >= kGraphInvalidIndex)
            return cxx::unexpected(RenderError{kDeviceCapacity});
        for (const auto& program : programs)
        {
            if (program.source)
            {
                const auto id = program.source.resource();
                if (!data_.canRead(*stream, id)) return cxx::unexpected(RenderError{kStorageMismatch, {slot.index}});
                if (data_.registry.hasExclusiveCommand(id))
                    return cxx::unexpected(RenderError{kDeviceBusy, {slot.index}});
                if (!program.source.available(data_.registry, data_.device))
                    return cxx::unexpected(RenderError{kStorageIdentity, {slot.index}});
            }
            if (program.shader.index >= shaders.size())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {slot.index}});
        }
        stream->programs.emplace(programs.begin(), programs.end());
        stream->shaders = std::move(shaders);
        return {};
    }

    RenderResult<void> RasterProgramBuildContext::reusePrograms(DrawStreamSlot slot) noexcept
    {
        auto* stream = data_.find(slot);
        if (!stream) return cxx::unexpected(RenderError{kGraphUnknownPass, {slot.index}});
        if (stream->programs || stream->reuse)
            return cxx::unexpected(RenderError{kGraphAmbiguousProducer, {slot.index}});
        if (!stream->ready) return cxx::unexpected(RenderError{kFrameContract, {slot.index}});
        stream->reuse = true;
        return {};
    }

    RenderResult<const void*>
    RasterProgramBuildContext::readResourceValue(
        DrawStreamSlot slot,
        ResourceInstanceId id,
        cxx::basic_type_info type
    ) const noexcept
    {
        auto* stream = data_.find(slot);
        if (!stream || !data_.canRead(*stream, id)) return cxx::unexpected(RenderError{kStorageMismatch, {slot.index}});
        if (data_.registry.hasExclusiveCommand(id)) return cxx::unexpected(RenderError{kDeviceBusy, {slot.index}});
        auto storage = data_.registry.borrow(id, type);
        if (!storage) return cxx::unexpected(storage.error());
        return *storage;
    }

    RevisionedValue<const void*> RasterProgramBuildContext::snapshot(std::size_t input) const noexcept
    {
        return data_.snapshots[input].selection;
    }
} // namespace lux::render
