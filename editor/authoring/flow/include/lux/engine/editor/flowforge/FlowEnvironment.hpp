#pragma once

#include <lux/engine/flowforge/graph/FlowSource.hpp>

namespace lux::editor::flowforge
{
    // One immutable catalog backing, shared by author construction, UI and compilation captures.
    // Span arrays are owned here; nested descriptors and code remain covered by the supplied owner.
    class FlowEnvironment final
    {
    public:
        explicit FlowEnvironment(lux::flowforge::FlowSourceEnvironment = {}, std::uint64_t version = 1);
        [[nodiscard]] lux::flowforge::FlowSourceEnvironment view() const noexcept;
        [[nodiscard]] std::uint64_t version() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
} // namespace lux::editor::flowforge
