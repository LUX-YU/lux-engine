#include <lux/engine/editor/widgets/NodeCanvas.hpp>
namespace lux::editor::widgets
{
    void CanvasDelete::operator()(ax::NodeEditor::EditorContext* context) const noexcept
    {
        ax::NodeEditor::DestroyEditor(context);
    }
    NodeCanvas createCanvas()
    {
        ax::NodeEditor::Config configuration;
        configuration.SettingsFile = nullptr;
        configuration.CanvasSizeMode = ax::NodeEditor::CanvasSizeMode::CenterOnly;
        return NodeCanvas{ax::NodeEditor::CreateEditor(&configuration)};
    }
    CanvasScope::CanvasScope(ax::NodeEditor::EditorContext* context) noexcept
        : previous_(ax::NodeEditor::GetCurrentEditor())
    {
        ax::NodeEditor::SetCurrentEditor(context);
    }
    CanvasScope::~CanvasScope() noexcept
    {
        ax::NodeEditor::SetCurrentEditor(previous_);
    }
}
