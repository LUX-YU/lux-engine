#include "PluginProbe.hpp"

namespace lux::editor::test
{
    lux::cxx::expected<std::reference_wrapper<PluginProbeTool>, ToolsetFailure>
    installPluginProbe(Toolset& toolset, PluginProbeState& state) noexcept
    {
        return toolset.install<PluginProbeTool>(state);
    }
} // namespace lux::editor::test
