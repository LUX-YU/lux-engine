#pragma once
#include <imgui_node_editor.h>
#include <memory>
namespace lux::editor::widgets
{
    struct CanvasDelete final
    {
        void operator()(ax::NodeEditor::EditorContext*) const noexcept;
    };
    using NodeCanvas = std::unique_ptr<ax::NodeEditor::EditorContext, CanvasDelete>;
    [[nodiscard]] NodeCanvas createCanvas();
    class CanvasScope final
    {
    public:
        explicit CanvasScope(ax::NodeEditor::EditorContext*) noexcept;
        ~CanvasScope() noexcept;
        CanvasScope(const CanvasScope&) = delete;
        CanvasScope& operator=(const CanvasScope&) = delete;
        CanvasScope(CanvasScope&&) = delete;
        CanvasScope& operator=(CanvasScope&&) = delete;

    private:
        ax::NodeEditor::EditorContext* previous_;
    };
}
