#pragma once
#include <lux/engine/editor/application/tooling/Toolset.hpp>
struct Counts { unsigned constructed{}, stopped{}, destroyed{}; };
struct ConsumerTool
{
    Counts &counts;
    explicit ConsumerTool(Counts &value) noexcept : counts(value) { ++counts.constructed; }
    void requestStop() noexcept { ++counts.stopped; }
    ~ConsumerTool() noexcept { ++counts.destroyed; }
};
#if defined(CONSUMER_TOOL_EXPORTS)
#define CONSUMER_TOOL_API __declspec(dllexport)
#else
#define CONSUMER_TOOL_API __declspec(dllimport)
#endif
CONSUMER_TOOL_API lux::cxx::expected<std::reference_wrapper<ConsumerTool>, lux::editor::application::ToolsetFailure>
installTool(lux::editor::application::Toolset &, Counts &) noexcept;
