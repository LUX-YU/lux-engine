#pragma once

#include <lux/engine/render/graph/Plan.hpp>

namespace lux::render
{
    struct GraphBackingTag;
    // Backend-owned, non-aliasing backing identity within the consumer's scope.
    // Graph never dereferences it or proves native lifetime/readiness.
    using GraphBackingId = cxx::StrongId<GraphBackingTag, std::uint64_t, 0>;

    struct GraphImportBinding
    {
        GraphResourceId resource;
        GraphBackingId backing;
        std::uint64_t dynamic_offset{0};
    };

    struct GraphFrameValues
    {
        std::uint64_t frame_serial{0};
        std::uint64_t render_time_ns{0};
        std::uint32_t frame_slot{0};
    };

    // Explicit borrow: plan and import storage must outlive this view and stay
    // unmodified/unmoved while it is consumed. No frame ownership or execution.
    class FrameGraphBindings
    {
    public:
        [[nodiscard]] static RenderResult<FrameGraphBindings> create(
            const CompiledGraphPlan& plan,
            GraphFrameValues frame,
            std::span<const GraphImportBinding> imports
        ) noexcept;

        static RenderResult<FrameGraphBindings> create(
            const CompiledGraphPlan&& plan,
            GraphFrameValues frame,
            std::span<const GraphImportBinding> imports
        ) = delete;

        [[nodiscard]] const CompiledGraphPlan& plan() const noexcept { return *plan_; }

        [[nodiscard]] GraphFrameValues frame() const noexcept { return frame_; }

        [[nodiscard]] std::span<const GraphImportBinding> imports() const noexcept { return imports_; }

    private:
        FrameGraphBindings(
            const CompiledGraphPlan& plan,
            GraphFrameValues frame,
            std::span<const GraphImportBinding> imports
        ) noexcept;

        const CompiledGraphPlan* plan_;
        GraphFrameValues frame_;
        std::span<const GraphImportBinding> imports_;
    };
}
