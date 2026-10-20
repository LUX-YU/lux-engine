#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <probe/visibility.hpp>
#include <string_view>
#include <vector>

namespace lux::render
{
    class RenderServer;
}

namespace probe
{
    struct Value final
    {
        std::uint64_t sequence;
        double amount;
    };

    struct SetValues final
    {
        static constexpr std::string_view name = "test.external.probe.set_values";

        struct Request final
        {
            std::vector<Value> values;
        };

        struct Response final
        {
            lux::render::RenderError error;
            std::uint64_t sequence = 0;
            double sum = 0;
        };
    };

    [[nodiscard]] PROBE_PUBLIC lux::render::RenderResult<lux::render::HandlerId> install(lux::render::
                                                                                             RenderServer&) noexcept;
} // namespace probe
