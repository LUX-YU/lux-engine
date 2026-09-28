#pragma once
#include <lux/engine/editor/flowforge/FlowAuthoringSource.hpp>
#include <lux/engine/editor/contracts/CodeLease.hpp>
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <variant>

namespace lux::editor::flowforge
{
    enum class EFlowEditError : std::uint8_t
    {
        INVALID_SOURCE,
        STALE_CONTENT,
        BUDGET,
        HISTORY,
        SESSION
    };
    struct FlowEditError final
    {
        EFlowEditError code{EFlowEditError::INVALID_SOURCE};
        lux::flowforge::FlowSourceFailure source;
        editing::EditFailure history;
        sessions::ESessionError session{sessions::ESessionError::INVALID_ARGUMENT};
        FlowEditError() = default;
        explicit FlowEditError(EFlowEditError value) : code(value) {}
        FlowEditError(sessions::ESessionError value) : code(EFlowEditError::SESSION), session(value) {}
    };
    template <class T> using FlowEditResult = lux::cxx::expected<T, FlowEditError>;

    struct FlowRename final
    {
        std::string value;
    };
    struct FlowSetLiteral final
    {
        lux::flowforge::PinId pin;
        lux::flowforge::FlowSourceLiteral value;
    };
    struct FlowInsertNode final
    {
        FlowInsertNode(
            contracts::CodeLease owner,
            std::unique_ptr<lux::flowforge::Node> node,
            lux::graph::GraphNodeLayout layout = {},
            bool preserve = false
        )
            : code(std::move(owner)), value(std::move(node)), placement(layout), preserve_ids(preserve)
        {}
        FlowInsertNode(FlowInsertNode&&) noexcept = default;
        FlowInsertNode& operator=(FlowInsertNode&& other) noexcept
        {
            using std::swap;
            swap(code, other.code);
            swap(value, other.value);
            swap(placement, other.placement);
            swap(preserve_ids, other.preserve_ids);
            return *this;
        }
        contracts::CodeLease code;
        std::unique_ptr<lux::flowforge::Node> value;
        lux::graph::GraphNodeLayout placement;
        bool preserve_ids{}; // Explicit source restoration; collision checks remain in FlowGraphEdit.
    };
    struct FlowInsertFunctionUse final
    {
        lux::flowforge::NodeId definition;
        bool return_node{};
        lux::graph::GraphNodeLayout placement;
    };
    struct FlowSetSignature final
    {
        lux::flowforge::NodeId node;
        std::string name;
        lux::flowforge::FlowSourceSignature value;
    };
    struct FlowSetExports final
    {
        std::vector<lux::flowforge::ExportMethodNode> value;
    };
    struct FlowRemoveNodes final
    {
        std::vector<lux::flowforge::NodeId> nodes;
        std::vector<lux::graph::LinkRecord> links;
    };
    struct FlowConnect final
    {
        lux::flowforge::PinId from, to;
    };
    struct FlowDisconnect final
    {
        lux::flowforge::PinId from, to;
    };
    struct FlowMoveNodes final
    {
        std::vector<lux::graph::GraphLayoutEntry> value;
    };
    struct FlowAddVariable final
    {
        std::string name, type;
        lux::flowforge::FlowSourceLiteral initial;
    };
    struct FlowSetVariable final
    {
        lux::flowforge::FlowSourceVariable value;
    };
    struct FlowRemoveVariable final
    {
        std::uint64_t id{};
    };
    using VFlowEdit = std::variant<
        FlowRename,
        FlowSetLiteral,
        FlowInsertNode,
        FlowInsertFunctionUse,
        FlowSetSignature,
        FlowSetExports,
        FlowRemoveNodes,
        FlowConnect,
        FlowDisconnect,
        FlowMoveNodes,
        FlowAddVariable,
        FlowSetVariable,
        FlowRemoveVariable>;
    struct FlowEditBatch final
    {
        sessions::ContentStamp expected;
        std::string label;
        std::vector<VFlowEdit> edits;
    };
    struct FlowEditIds final
    {
        std::vector<lux::flowforge::NodeId> nodes;
        std::vector<std::uint64_t> variables;
    };
    struct FlowEditReceipt final
    {
        editing::EEditEffect effect{};
        sessions::ContentStamp content;
        sessions::ObservationVersion observed;
        FlowEditIds inserted;
    };
    struct FlowEditObserver final
    {
        void* owner{};
        void (*changed)(void*, const editing::CommitInfo&, bool structural) noexcept {};
    };
    struct PreparedFlowEdit final
    {
        editing::EditOperationPtr operation;
        // Valid while operation is alive (including after History adopts it). Copy immediately after execute.
        const FlowEditIds* inserted{};
    };
    // Pure preparation shared with the expiring product adapter. Admission encloses this call,
    // History::execute and all input/temporary destruction. It never owns another History.
    [[nodiscard]] editing::EditResult<PreparedFlowEdit> prepareFlowEdit(
        FlowAuthoringSource& source,
        lux::flowforge::FlowSourceEnvironment environment,
        editing::StateId base,
        std::vector<VFlowEdit> edits,
        std::string label,
        contracts::CodeLease code,
        FlowEditObserver observer
    );

    // P12 product adapter: caller retains a rejected input and keeps it alive through execute.
    [[nodiscard]] editing::EditResult<PreparedFlowEdit> prepareBorrowedFlowInsert(
        FlowAuthoringSource& source,
        lux::flowforge::FlowSourceEnvironment environment,
        editing::StateId base,
        std::unique_ptr<lux::flowforge::Node>& input,
        lux::graph::GraphNodeLayout placement,
        FlowEditObserver observer
    );
}
