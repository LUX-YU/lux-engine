#pragma once
#include <lux/engine/editor/widgets/NodeCanvas.hpp>
#include <lux/engine/editor/widgets/NodeCanvasIds.hpp>
#include <lux/engine/ui/Element.hpp>
#include <variant>
#include <span>

namespace lux::editor::widgets
{
    struct CanvasPin final
    {
        std::uint64_t id{};
        std::string label;
        bool input{};
    };
    struct CanvasNode final
    {
        std::uint64_t id{};
        std::string label;
        std::vector<CanvasPin> pins;
        lux::ui::Point position;
        bool placed{};
    };
    struct CanvasLink final
    {
        std::uint64_t from{}, to{};
        friend bool operator==(CanvasLink, CanvasLink) = default;
    };
    struct CanvasPosition final
    {
        std::uint64_t node{};
        lux::ui::Point position;
    };
    struct CanvasErase final
    {
        std::vector<std::uint64_t> nodes;
        std::vector<CanvasLink> links;
    };
    struct CanvasMove final
    {
        std::vector<CanvasPosition> nodes;
    };
    using VCanvasEdit = std::variant<CanvasLink, CanvasErase, CanvasMove>;
    struct CanvasEdit final
    {
        VCanvasEdit value;
        bool began{}, committed{}, cancelled{};
    };
    // Display values only; identities remain opaque to the canvas. The domain owns validation and History.
    class GraphCanvas final : public lux::ui::Element
    {
    public:
        object::TSignal<CanvasEdit> edited{*this};
        object::TSignal<std::span<const std::uint64_t>> selected{*this};
        GraphCanvas(lux::ui::Element&, lux::ui::ElementId);
        GraphCanvas(const GraphCanvas&) = delete;
        GraphCanvas& operator=(const GraphCanvas&) = delete;
        GraphCanvas(GraphCanvas&&) = delete;
        GraphCanvas& operator=(GraphCanvas&&) = delete;
        // Owner calls only after the preceding gesture has ended. Node-editor retains pan/zoom locally.
        void setGraph(std::vector<CanvasNode>, std::vector<CanvasLink>);
        [[nodiscard]] std::span<const CanvasNode> nodes() const noexcept
        {
            return nodes_;
        }
        [[nodiscard]] object::SignalDelivery delivery() const noexcept
        {
            return delivery_;
        }
        void finishEdit(bool cancel = false) noexcept override;

    private:
        void draw() noexcept override;
        void publish(CanvasEdit) noexcept;
        NodeCanvas canvas_;
        NodeCanvasIds ids_;
        std::vector<CanvasNode> nodes_;
        std::vector<CanvasLink> links_;
        std::vector<std::uint64_t> selection_;
        std::vector<ax::NodeEditor::NodeId> selected_;
        CanvasMove movement_;
        bool moving_{};
        object::SignalDelivery delivery_;
    };
}
