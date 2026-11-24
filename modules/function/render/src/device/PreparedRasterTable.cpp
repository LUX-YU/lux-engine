#include <lux/engine/render/device/PreparedRasterTable.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/cxx/core/scope_exit.hpp>

namespace lux::render::detail
{
    PreparedRasterTable::~PreparedRasterTable() noexcept
    {
        const auto identity = [&](std::size_t i) noexcept { return programs_[i]->cache_id; };
        device_.session().programs().release(programs_.size(), identity, {});
    }

    exec::task<RenderResult<void>> PreparedRasterTable::compile(std::vector<RasterProgramLink> recipes) noexcept
    {
        auto& workers = device_.workers();
        cxx::scope_exit finish([&]() noexcept { workers.endCompilation(); });
        co_await workers.beginCompilation();
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        programs_.reserve(recipes.size());
        auto& native = device_.session().programs();
        for (auto& recipe : recipes)
        {
            if (const auto error = device_.admissionError(); error.type) co_return cxx::unexpected(error);
            if (stop.stop_requested()) co_return cxx::unexpected(RenderError{kCommandCancelled});

            auto ready = co_await native.prepareSelectedProgram(
                std::move(recipe.input), workers.programScheduler(), stop
            );
            if (!ready) co_return cxx::unexpected(ready.error());
            auto linked = std::move(recipe).finish(std::move(*ready));
            programs_.push_back(std::make_shared<const CompiledRasterProgram>(std::move(linked)));
        }
        if (const auto error = device_.admissionError(); error.type) co_return cxx::unexpected(error);
        if (stop.stop_requested()) co_return cxx::unexpected(RenderError{kCommandCancelled});
        co_return RenderResult<void>{};
    }
} // namespace lux::render::detail
