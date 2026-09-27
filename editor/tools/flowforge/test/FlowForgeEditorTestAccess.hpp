#pragma once
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>

namespace lux::editor::flowforge
{
    class LUX_EDITOR_FLOWFORGE_PUBLIC FlowForgeEditorTestAccess final
    {
    public:
        explicit FlowForgeEditorTestAccess(FlowForgeEditor& tool) noexcept : tool_(tool) {}
        [[nodiscard]] EditorResult<lux::flowforge::FlowSource> capture() const;
        [[nodiscard]] EditorResult<lux::flowforge::FlowSourceNode> captureNode(lux::flowforge::NodeId) const;
        [[nodiscard]] editing::EditResult<editing::ApplyResult>
        setFunctionSignature(editing::StateId, lux::flowforge::NodeId, std::string_view name, const lux::flowforge::FlowSourceSignature&);
        [[nodiscard]] editing::EditResult<lux::flowforge::NodeId> insertFunctionUse(
            lux::flowforge::NodeId definition,
            bool return_node,
            lux::graph::GraphNodeLayout = {}
        );
        [[nodiscard]] std::span<const lux::flowforge::ExportMethodNode> exports() const noexcept;
        [[nodiscard]] ProjectStorage& project() noexcept;
        [[nodiscard]] const lux::flowforge::FlowSourceEnvironment& metadata() const noexcept;
        [[nodiscard]] std::span<const lux::graph::NodeRecord> nodes() const noexcept;
        [[nodiscard]] std::span<const lux::graph::PinRecord> pins() const noexcept;
        [[nodiscard]] std::span<const lux::graph::LinkRecord> links() const noexcept;
        [[nodiscard]] std::string_view nodeName(lux::flowforge::NodeId) const noexcept;
        [[nodiscard]] lux::flowforge::ENodeOperation nodeOperation(lux::flowforge::NodeId) const noexcept;
        [[nodiscard]] std::string_view pinName(lux::flowforge::PinId) const noexcept;
        [[nodiscard]] std::string_view pinType(lux::flowforge::PinId) const noexcept;
        [[nodiscard]] EditorResult<lux::flowforge::FlowSourceLiteral> pinLiteral(lux::flowforge::PinId) const;
        [[nodiscard]] editing::EditResult<editing::ApplyResult>
        setPinLiteral(lux::flowforge::PinId, const lux::flowforge::FlowSourceLiteral&);
        [[nodiscard]] lux::graph::GraphNodeLayout nodeLayout(lux::flowforge::NodeId) const noexcept;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> rename(std::string_view);
        [[nodiscard]] std::span<const lux::flowforge::FlowGraph::GraphVariable> variables() const noexcept;
        [[nodiscard]] editing::EditResult<std::uint64_t> addVariable(
            std::string_view name,
            std::string_view type,
            const lux::flowforge::FlowSourceLiteral& initial
        );
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setVariable(const lux::flowforge::FlowSourceVariable&);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> removeVariable(std::uint64_t);
        [[nodiscard]] editing::EditResult<lux::flowforge::NodeId> insertNode(
            std::unique_ptr<lux::flowforge::Node>&,
            lux::graph::GraphNodeLayout = {}
        );
        [[nodiscard]] editing::EditResult<editing::ApplyResult> removeNodes(
            std::span<const lux::flowforge::NodeId>,
            std::span<const lux::graph::LinkRecord> links = {}
        );
        [[nodiscard]] editing::EditResult<editing::ApplyResult> connect(
            lux::flowforge::PinId from,
            lux::flowforge::PinId to
        );
        [[nodiscard]] editing::EditResult<editing::ApplyResult> disconnect(
            lux::flowforge::PinId from,
            lux::flowforge::PinId to
        );
        [[nodiscard]] editing::EditResult<
            editing::ApplyResult> moveNodes(std::span<const lux::graph::GraphLayoutEntry>);
        [[nodiscard]] editing::EditResult<
            editing::ApplyResult> setExports(std::vector<lux::flowforge::ExportMethodNode>);

    private:
        FlowForgeEditor& tool_;
    };
}
inline auto toolTest(lux::editor::flowforge::FlowForgeEditor& tool) noexcept
{
    return lux::editor::flowforge::FlowForgeEditorTestAccess{tool};
}
inline auto toolTest(const lux::editor::flowforge::FlowForgeEditor& tool) noexcept
{
    return lux::editor::flowforge::FlowForgeEditorTestAccess{const_cast<lux::editor::flowforge::FlowForgeEditor&>(tool)
    };
}
