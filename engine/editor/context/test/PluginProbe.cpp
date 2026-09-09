#include "PluginProbe.hpp"

namespace lux::editor::test
{
    lux::cxx::expected<std::reference_wrapper<PluginProbeTool>, application::ToolsetFailure> installPluginProbe(
        application::Toolset& toolset, PluginProbeState& state
    ) noexcept
    {
        return toolset.install<PluginProbeTool>(state);
    }
} // namespace lux::editor::test
