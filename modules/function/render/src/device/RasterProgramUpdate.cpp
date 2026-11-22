#include <lux/engine/render/device/RasterProgramUpdate.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <algorithm>

namespace lux::render::detail
{
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
