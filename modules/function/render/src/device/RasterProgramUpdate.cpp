#include <lux/engine/render/device/RasterProgramUpdate.hpp>
#include <lux/engine/render/commands/Error.hpp>

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
            for (auto& link : stream.links)
            {
                if (stop.stop_requested()) co_return cxx::unexpected(RenderError{kCommandCancelled});
                auto ready = co_await device.programs().prepareSelectedProgram(
                    std::move(link.input), cpu, stop.get_token()
                );
                if (!ready) co_return cxx::unexpected(ready.error());
                stream.candidate->programs.push_back(std::move(link).finish(std::move(*ready)));
            }
        }
        co_return RenderResult<void>{};
    }
} // namespace lux::render::detail
