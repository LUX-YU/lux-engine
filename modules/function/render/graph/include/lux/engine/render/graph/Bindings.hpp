#pragma once

#include <lux/engine/render/graph/Plan.hpp>
#include <ranges>

namespace lux::render
{
    struct GraphBackingTag;
    using GraphBackingId = cxx::StrongId<GraphBackingTag, std::uint64_t, 0>;

    struct GraphImportBinding
    {
        GraphResourceId resource;
        GraphBackingId backing;
        std::uint64_t dynamic_offset{};
        // Explicit external-owner promise for this invocation. Native fence/pin proof belongs to F4.
        std::uint64_t ready_epoch{};
        std::uint64_t history_epoch{};
        bool operator==(const GraphImportBinding&) const noexcept = default;
    };

    struct GraphFrameValues
    {
        std::uint64_t frame_serial{}, render_time_ns{};
        std::uint32_t frame_slot{};
    };

    struct ColorClearValue
    {
        std::array<float, 4> value;
        bool operator==(const ColorClearValue&) const noexcept = default;
    };

    struct DepthStencilClearValue
    {
        float depth;
        std::uint32_t stencil;
        bool operator==(const DepthStencilClearValue&) const noexcept = default;
    };

    using VGraphDynamicField = std::variant<std::monostate, GraphSampler, ColorClearValue, DepthStencilClearValue>;

    struct GraphPassInvocation
    {
        std::vector<std::byte> scalars;
        std::vector<VGraphDynamicField> fields;
        bool enabled{true};
        bool operator==(const GraphPassInvocation&) const noexcept = default;
    };

    struct GraphInvocationData
    {
        std::vector<GraphPassInvocation> passes;
        std::uint64_t scene{}, scene_revision{}, view{}, target{}, history_epoch{};
        std::array<float, 16> camera{};
    };

    [[nodiscard]] GraphInvocationData makeGraphInvocationData(const RenderGraphDefinition& definition) noexcept;

    template <typename T>
    concept GraphImportStorage = std::ranges::contiguous_range<T> && std::ranges::sized_range<T> &&
                                 std::same_as<std::ranges::range_value_t<T>, GraphImportBinding>;

    // Synchronous borrow only: named plan, imports and invocation storage must remain alive and unmoved.
    // No rvalue range/Plan overload. Constructing an already dangling named span violates its own contract.
    class FrameGraphBindings
    {
    public:
        template <GraphImportStorage T>
        [[nodiscard]] static RenderResult<FrameGraphBindings> create(
            const LogicalGraphPlan& plan,
            GraphFrameValues frame,
            T& imports,
            const GraphInvocationData& values
        ) noexcept
        {
            return validate(plan, frame, std::span<const GraphImportBinding>(imports), &values);
        }

        template <GraphImportStorage T>
        [[nodiscard]] static RenderResult<FrameGraphBindings> create(
            const LogicalGraphPlan& plan,
            GraphFrameValues frame,
            T& imports
        ) noexcept
        {
            return validate(plan, frame, std::span<const GraphImportBinding>(imports), nullptr);
        }

        template <GraphImportStorage T>
        static RenderResult<FrameGraphBindings> create(const LogicalGraphPlan&&, GraphFrameValues, T&) = delete;
        template <GraphImportStorage T>
        static RenderResult<FrameGraphBindings>
        create(const LogicalGraphPlan&&, GraphFrameValues, T&, const GraphInvocationData&) = delete;
        template <GraphImportStorage T>
        static RenderResult<FrameGraphBindings>
        create(const LogicalGraphPlan&, GraphFrameValues, T&, const GraphInvocationData&&) = delete;

        [[nodiscard]] const LogicalGraphPlan& plan() const noexcept
        {
            return *plan_;
        }

        [[nodiscard]] const GraphFrameValues& frame() const noexcept
        {
            return frame_;
        }

        [[nodiscard]] std::span<const GraphImportBinding> imports() const noexcept
        {
            return imports_;
        }

        [[nodiscard]] const GraphInvocationData* values() const noexcept
        {
            return values_;
        }

        [[nodiscard]] GraphResourceId resourceFor(GraphPassId pass, std::uint32_t use_index) const noexcept;

        [[nodiscard]] const GraphPassInvocation* invocation(GraphPassId pass) const noexcept
        {
            return values_ && pass.isValid() && pass.value() <= values_->passes.size()
                       ? &values_->passes[pass.value() - 1]
                       : nullptr;
        }

    private:
        static RenderResult<FrameGraphBindings> validate(
            const LogicalGraphPlan& plan,
            GraphFrameValues frame,
            std::span<const GraphImportBinding> imports,
            const GraphInvocationData* values
        ) noexcept;

        FrameGraphBindings(
            const LogicalGraphPlan& plan,
            GraphFrameValues frame,
            std::span<const GraphImportBinding> imports,
            const GraphInvocationData* values
        ) noexcept
            : plan_(&plan), frame_(frame), imports_(imports), values_(values)
        {
        }

        const LogicalGraphPlan* plan_;
        GraphFrameValues frame_;
        std::span<const GraphImportBinding> imports_;
        const GraphInvocationData* values_;
    };

    [[nodiscard]] bool mayShareSceneInvocation(
        GraphPassId pass,
        const FrameGraphBindings& a,
        const FrameGraphBindings& b
    ) noexcept;
} // namespace lux::render
