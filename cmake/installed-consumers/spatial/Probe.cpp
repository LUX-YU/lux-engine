#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <probe/Probe.hpp>

namespace probe
{
    namespace
    {
        void setValues(const SetValues::Request& input, SetValues::Response& output) noexcept
        {
            output = {};
            for (const auto& value : input.values)
            {
                output.sequence = value.sequence;
                output.sum += value.amount;
            }
        }
    }

    lux::render::RenderResult<void> install(lux::render::RenderServer& server) noexcept
    {
        const std::array bindings{lux::render::CommandBinding::declare<SetValues, &setValues>()};
        return server.registerCommands(bindings);
    }
} // namespace probe
