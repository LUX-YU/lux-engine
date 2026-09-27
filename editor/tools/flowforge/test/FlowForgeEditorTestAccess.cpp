#include "FlowForgeEditorTestAccess.hpp"
#include <lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp>

namespace lux::editor::flowforge
{
    EditorResult<lux::flowforge::FlowSource> FlowForgeEditorTestAccess::capture() const
    {
        return tool_.impl_->capture();
    }

    EditorResult<lux::flowforge::FlowSourceNode> FlowForgeEditorTestAccess::captureNode(lux::flowforge::NodeId id) const
    {
        return tool_.impl_->captureNode(std::move(id));
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::setFunctionSignature(
        editing::StateId base,
        lux::flowforge::NodeId id,
        std::string_view name,
        const lux::flowforge::FlowSourceSignature& signature
    )
    {
        return tool_.impl_->setFunctionSignature(std::move(base), std::move(id), std::move(name), signature);
    }

    editing::EditResult<lux::flowforge::NodeId> FlowForgeEditorTestAccess::insertFunctionUse(
        lux::flowforge::NodeId id,
        bool return_node,
        lux::graph::GraphNodeLayout layout
    )
    {
        return tool_.impl_->insertFunctionUse(std::move(id), std::move(return_node), std::move(layout));
    }

    std::span<const lux::flowforge::ExportMethodNode> FlowForgeEditorTestAccess::exports() const noexcept
    {
        return tool_.impl_->exports();
    }

    ProjectStorage& FlowForgeEditorTestAccess::project() noexcept
    {
        return tool_.impl_->project();
    }

    const lux::flowforge::FlowSourceEnvironment& FlowForgeEditorTestAccess::metadata() const noexcept
    {
        return tool_.impl_->metadata();
    }

    std::span<const lux::graph::NodeRecord> FlowForgeEditorTestAccess::nodes() const noexcept
    {
        return tool_.impl_->nodes();
    }

    std::span<const lux::graph::PinRecord> FlowForgeEditorTestAccess::pins() const noexcept
    {
        return tool_.impl_->pins();
    }

    std::span<const lux::graph::LinkRecord> FlowForgeEditorTestAccess::links() const noexcept
    {
        return tool_.impl_->links();
    }

    std::string_view FlowForgeEditorTestAccess::nodeName(lux::flowforge::NodeId id) const noexcept
    {
        return tool_.impl_->nodeName(std::move(id));
    }

    lux::flowforge::ENodeOperation FlowForgeEditorTestAccess::nodeOperation(lux::flowforge::NodeId id) const noexcept
    {
        return tool_.impl_->nodeOperation(std::move(id));
    }

    std::string_view FlowForgeEditorTestAccess::pinName(lux::flowforge::PinId id) const noexcept
    {
        return tool_.impl_->pinName(std::move(id));
    }

    std::string_view FlowForgeEditorTestAccess::pinType(lux::flowforge::PinId id) const noexcept
    {
        return tool_.impl_->pinType(std::move(id));
    }

    EditorResult<lux::flowforge::FlowSourceLiteral> FlowForgeEditorTestAccess::pinLiteral(lux::flowforge::PinId id
    ) const
    {
        return tool_.impl_->pinLiteral(std::move(id));
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::setPinLiteral(
        lux::flowforge::PinId id,
        const lux::flowforge::FlowSourceLiteral& literal
    )
    {
        return tool_.impl_->setPinLiteral(std::move(id), literal);
    }

    lux::graph::GraphNodeLayout FlowForgeEditorTestAccess::nodeLayout(lux::flowforge::NodeId id) const noexcept
    {
        return tool_.impl_->nodeLayout(std::move(id));
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::rename(std::string_view name)
    {
        return tool_.impl_->rename(std::move(name));
    }

    std::span<const lux::flowforge::FlowGraph::GraphVariable> FlowForgeEditorTestAccess::variables() const noexcept
    {
        return tool_.impl_->variables();
    }

    editing::EditResult<std::uint64_t> FlowForgeEditorTestAccess::addVariable(
        std::string_view name,
        std::string_view type,
        const lux::flowforge::FlowSourceLiteral& initial
    )
    {
        return tool_.impl_->addVariable(std::move(name), std::move(type), initial);
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::setVariable(
        const lux::flowforge::FlowSourceVariable& value
    )
    {
        return tool_.impl_->setVariable(value);
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::removeVariable(std::uint64_t id)
    {
        return tool_.impl_->removeVariable(std::move(id));
    }

    editing::EditResult<lux::flowforge::NodeId> FlowForgeEditorTestAccess::insertNode(
        std::unique_ptr<lux::flowforge::Node>& input,
        lux::graph::GraphNodeLayout placement
    )
    {
        return tool_.impl_->insertNode(input, std::move(placement));
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::removeNodes(
        std::span<const lux::flowforge::NodeId> nodes,
        std::span<const lux::graph::LinkRecord> links
    )
    {
        return tool_.impl_->removeNodes(std::move(nodes), std::move(links));
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::connect(
        lux::flowforge::PinId from,
        lux::flowforge::PinId to
    )
    {
        return tool_.impl_->connect(std::move(from), std::move(to));
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::disconnect(
        lux::flowforge::PinId from,
        lux::flowforge::PinId to
    )
    {
        return tool_.impl_->disconnect(std::move(from), std::move(to));
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::moveNodes(
        std::span<const lux::graph::GraphLayoutEntry> entries
    )
    {
        return tool_.impl_->moveNodes(std::move(entries));
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditorTestAccess::setExports(
        std::vector<lux::flowforge::ExportMethodNode> exports
    )
    {
        return tool_.impl_->setExports(std::move(exports));
    }
}
