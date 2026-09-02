#pragma once

#include <lux/engine/editor/Toolset.hpp>

#include <functional>

namespace lux::editor::test
{
    struct PluginProbeState final
    {
        int stop_count{};
        int destruction_count{};
    };

    class PluginProbeTool final
    {
    public:
        explicit PluginProbeTool(PluginProbeState& state) noexcept : state_(state)
        {
        }

        ~PluginProbeTool() noexcept
        {
            ++state_.destruction_count;
        }

        void requestStop() noexcept
        {
            ++state_.stop_count;
        }

        [[nodiscard]] int value() const noexcept
        {
            return 42;
        }

    private:
        PluginProbeState& state_;
    };

    [[nodiscard]] lux::cxx::expected<std::reference_wrapper<PluginProbeTool>, ToolsetFailure>
    installPluginProbe(Toolset& toolset, PluginProbeState& state) noexcept;
} // namespace lux::editor::test
