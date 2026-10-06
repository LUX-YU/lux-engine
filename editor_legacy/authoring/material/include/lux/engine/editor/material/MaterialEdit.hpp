#pragma once

#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <array>
#include <variant>

namespace lux::editor::material
{
    enum class EMaterialEditError : std::uint8_t
    {
        INVALID_SOURCE,
        INVALID_NODE,
        INVALID_VALUE,
        INVALID_GRAPH,
        REFERENCE_IN_USE,
        STALE_CONTENT,
        BUDGET,
        CALLBACK,
        HISTORY,
        SESSION
    };
    struct MaterialEditError final
    {
        EMaterialEditError code{EMaterialEditError::INVALID_SOURCE};
        lux::material::NodeId node;
        lux::graph::GraphTopologyFailure graph;
        editing::EditFailure history;
        sessions::ESessionError session{sessions::ESessionError::INVALID_ARGUMENT};
        MaterialEditError() = default;
        explicit MaterialEditError(EMaterialEditError value, lux::material::NodeId id = {}) : code(value), node(id) {}
        MaterialEditError(sessions::ESessionError value) : code(EMaterialEditError::SESSION), session(value) {}
    };
    template <class T> using MaterialEditResult = lux::cxx::expected<T, MaterialEditError>;

    struct MaterialRename final
    {
        std::string value;
    };
    struct MaterialSetConstant final
    {
        lux::material::NodeId node;
        std::array<float, 4> value;
    };
    struct MaterialSetShading final
    {
        lux::rdesc::ELightingTechnique value;
    };
    struct MaterialSetRenderState final
    {
        lux::material::RenderState value;
    };
    struct MaterialSetTextureSlots final
    {
        std::vector<lux::material::TextureSlotDecl> value;
    };
    struct MaterialSetParameterSlots final
    {
        std::vector<lux::material::ParamSlotDecl> value;
    };
    // By-value batches own candidates even on failure. Live content acquires only prepared clones.
    // The outer lease outlives the virtual destructor and allocator of the candidate.
    struct MaterialInsertNode final
    {
        MaterialInsertNode(
            lux::object::CodeLease owner,
            std::unique_ptr<lux::material::Node> node,
            lux::graph::GraphNodeLayout layout = {}
        )
            : code(std::move(owner)), value(std::move(node)), placement(layout)
        {}
        MaterialInsertNode(MaterialInsertNode&&) noexcept = default;
        MaterialInsertNode& operator=(MaterialInsertNode&& other) noexcept
        {
            using std::swap;
            swap(code, other.code);
            swap(value, other.value);
            swap(placement, other.placement);
            return *this;
        }
        lux::object::CodeLease code{lux::object::CodeLease::builtin()};
        std::unique_ptr<lux::material::Node> value;
        lux::graph::GraphNodeLayout placement;
    };
    struct MaterialReplaceNode final
    {
        MaterialReplaceNode(lux::object::CodeLease owner, std::unique_ptr<lux::material::Node> node)
            : code(std::move(owner)), value(std::move(node))
        {}
        MaterialReplaceNode(MaterialReplaceNode&&) noexcept = default;
        MaterialReplaceNode& operator=(MaterialReplaceNode&& other) noexcept
        {
            using std::swap;
            swap(code, other.code);
            swap(value, other.value);
            return *this;
        }
        lux::object::CodeLease code{lux::object::CodeLease::builtin()};
        std::unique_ptr<lux::material::Node> value;
    };
    struct MaterialEraseNode final
    {
        lux::material::NodeId node;
    };
    struct MaterialConnect final
    {
        lux::material::PinId from, to;
    };
    struct MaterialDisconnect final
    {
        lux::material::PinId from, to;
    };
    struct MaterialPlaceNode final
    {
        lux::material::NodeId node;
        lux::graph::GraphNodeLayout value;
    };
    using VMaterialEdit = std::variant<
        MaterialRename,
        MaterialSetConstant,
        MaterialSetShading,
        MaterialSetRenderState,
        MaterialSetTextureSlots,
        MaterialSetParameterSlots,
        MaterialInsertNode,
        MaterialReplaceNode,
        MaterialEraseNode,
        MaterialConnect,
        MaterialDisconnect,
        MaterialPlaceNode>;
    struct MaterialEditBatch final
    {
        sessions::ContentStamp expected;
        std::string label;
        std::vector<VMaterialEdit> edits;
    };
    struct MaterialEditReceipt final
    {
        editing::EEditEffect effect{};
        sessions::ContentStamp content;
        sessions::ObservationVersion observed;
        std::vector<lux::material::NodeId> inserted;
    };

    // Pure algorithm shared by the session and the expiring product adapter. The caller owns source
    // and History and must hold its edit admission throughout preparation, execute and reclamation.
    struct MaterialEditObserver final
    {
        void* owner{};
        void (*changed)(void*, const editing::CommitInfo&) noexcept {};
    };
    struct PreparedMaterialEdit final
    {
        editing::EditOperationPtr operation;
        std::vector<lux::material::NodeId> inserted;
    };
    [[nodiscard]] MaterialEditResult<PreparedMaterialEdit> prepareMaterialEdit(
        lux::material::MaterialSource& source,
        editing::StateId base,
        std::vector<VMaterialEdit> edits,
        std::string label,
        lux::object::CodeLease code,
        MaterialEditObserver observer,
        std::size_t staging_limit
    );
}
