#include "Tool.hpp"
lux::cxx::expected<std::reference_wrapper<ConsumerTool>, lux::editor::application::ToolsetFailure>
installTool(lux::editor::application::Toolset &tools, Counts &counts) noexcept
{
    return tools.install<ConsumerTool>(counts);
}
