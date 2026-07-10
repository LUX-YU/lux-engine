#include "panels/FlowGraphPanel.hpp"

#include <unordered_map>
#include <utility>

#include <imgui.h>

#include <lux/engine/editor/panels/ConstantEditor.hpp>
#include <lux/engine/flowforge/ControlNode.hpp>
#include <lux/engine/flowforge/FunctionalNode.hpp>
#include <lux/engine/flowforge/ObjectNode.hpp>
#include <lux/engine/ecs/DebugDraw.hpp>

#if LUX_FLOWFORGE_HAS_MLIR
#include <imgui_stdlib.h>
#include <lux/engine/flowforge/mlir/IR.hpp>
#include <lux/engine/flowforge/mlir/Passes.hpp>
#endif

namespace
{
    /// Output lines captured from JITed graph code (the run-button path keeps
    /// JIT execution synchronous on the UI thread, so no locking needed).
    std::vector<std::string> g_flow_run_output;
}

/// The host function behind the Hello World graph's Print node. The JITed
/// @main calls it through the NATIVE_FUNC_CALL extern declaration, bound via
/// runMainJIT's symbol table (ORC does not search process exports on Windows).
extern "C" void lux_flow_print(int value)
{
    g_flow_run_output.push_back("lux_flow_print(" + std::to_string(value) + ")");
}

namespace lux::editor
{
    namespace gk = lux::graphkit;
    namespace ff = lux::flowforge;

    namespace
    {
        // ---- flow domain pin-type encoding (opaque draw token) ---------------
        constexpr std::uint32_t kFlowDomainTag    = 0x46u << 24; // 'F'
        constexpr std::uint32_t kFlowExecCategory = kFlowDomainTag | 0x1u;

        std::uint32_t dataCategoryOf(const lux::meta::RefType* type)
        {
            const std::uint64_t h64 = type ? type->hash : 0ull;
            const auto h16 = static_cast<std::uint32_t>((h64 ^ (h64 >> 32)) & 0xFFFFull);
            return kFlowDomainTag | 0x2u | (h16 << 4);
        }

        gk::GraphPinType pinTypeOf(const ff::Pin* pin)
        {
            gk::GraphPinType t;
            switch (pin->kind())
            {
            case ff::EPinKind::EXEC_IN:
                t.category  = kFlowExecCategory;
                t.direction = gk::EPinSide::INPUT;
                t.fan_cap   = gk::kFanUnlimited; // exec join: fan-in N
                break;
            case ff::EPinKind::EXEC_OUT:
                t.category  = kFlowExecCategory;
                t.direction = gk::EPinSide::OUTPUT;
                t.fan_cap   = 1; // single next_pin_
                break;
            case ff::EPinKind::DATA_IN:
                t.category  = dataCategoryOf(static_cast<const ff::DataInPin*>(pin)->info().type);
                t.direction = gk::EPinSide::INPUT;
                t.fan_cap   = 1;
                break;
            case ff::EPinKind::DATA_OUT:
            default:
                t.category  = dataCategoryOf(static_cast<const ff::DataOutPin*>(pin)->info().type);
                t.direction = gk::EPinSide::OUTPUT;
                t.fan_cap   = gk::kFanUnlimited;
                break;
            }
            return t;
        }

        bool pinHasLink(const ff::Pin* pin)
        {
            switch (pin->kind())
            {
            case ff::EPinKind::EXEC_IN:
                return !static_cast<const ff::ExecInPin*>(pin)->linkedPins().empty();
            case ff::EPinKind::EXEC_OUT:
                return static_cast<const ff::ExecOutPin*>(pin)->nextPin() != nullptr;
            case ff::EPinKind::DATA_IN:
                return static_cast<const ff::DataInPin*>(pin)->linkedPin() != nullptr;
            case ff::EPinKind::DATA_OUT:
            default:
                return !static_cast<const ff::DataOutPin*>(pin)->linkPins().empty();
            }
        }

        /// Canvas-position record stored in NodeStorage.user_data — the flow
        /// domain's editor-metadata slot (persisted by LFGR in P3).
        struct FlowNodeEditorMeta
        {
            gk::GraphVec2 pos{};
            bool          placed = false;
        };

        FlowNodeEditorMeta* metaOf(ff::NodeStorage& storage)
        {
            return static_cast<FlowNodeEditorMeta*>(storage.user_data.get());
        }

        std::uint32_t outPinIndexOf(const ff::Node* owner, const ff::Pin* pin)
        {
            const auto& outs = owner->outPins();
            for (std::uint32_t i = 0; i < outs.size(); ++i)
            {
                if (outs[i] == pin)
                {
                    return i;
                }
            }
            return gk::kInvalidPin;
        }
    } // namespace

    // ======================== FlowGraphView =================================

    void FlowGraphView::forEachNode(
        const std::function<void(gk::GraphNodeRef, std::string_view)>& fn) const
    {
        for (const ff::NodeStorage& storage : graph_->nodes())
        {
            if (!storage.node)
            {
                continue;
            }
            const std::string& name = storage.node->name();
            fn(gk::GraphNodeRef{ storage.index },
               name.empty() ? std::string_view{ ff::toString(storage.node->operation()) }
                            : std::string_view{ name });
        }
    }

    std::uint32_t FlowGraphView::pinCount(gk::GraphNodeRef node, gk::EPinSide side) const
    {
        if (!graph_->hasNode(node.id))
        {
            return 0;
        }
        const ff::Node* n = graph_->getNode(node.id).node.get();
        return static_cast<std::uint32_t>(side == gk::EPinSide::INPUT ? n->inPins().size()
                                                                      : n->outPins().size());
    }

    ff::Pin* FlowGraphView::pinAt(gk::GraphPinRef ref) const
    {
        if (!ref.valid() || !graph_->hasNode(ref.node.id))
        {
            return nullptr;
        }
        ff::Node* n = graph_->getNode(ref.node.id).node.get();
        auto& pins = ref.side == gk::EPinSide::INPUT ? n->inPins() : n->outPins();
        return ref.pin < pins.size() ? pins[ref.pin] : nullptr;
    }

    gk::GraphPinView FlowGraphView::pin(gk::GraphNodeRef node, gk::EPinSide side,
                                        std::uint32_t index) const
    {
        const ff::Pin* p = pinAt(gk::GraphPinRef{ node, side, index });
        if (!p)
        {
            return {};
        }
        return gk::GraphPinView{ p->name(), pinTypeOf(p), pinHasLink(p) };
    }

    void FlowGraphView::forEachLink(
        const std::function<void(gk::GraphLinkView)>& fn) const
    {
        // Pins hold raw Node* back-refs; resolve owners to sparse indices once.
        std::unordered_map<const ff::Node*, std::size_t> owner_index;
        for (const ff::NodeStorage& storage : graph_->nodes())
        {
            owner_index.emplace(storage.node.get(), storage.index);
        }

        const auto emit = [&](const ff::Node* src_owner, const ff::Pin* src_pin,
                              std::size_t dst_index, std::uint32_t dst_pin)
        {
            const auto it = owner_index.find(src_owner);
            if (it == owner_index.end())
            {
                return;
            }
            const std::uint32_t src_idx = outPinIndexOf(src_owner, src_pin);
            if (src_idx == gk::kInvalidPin)
            {
                return;
            }
            fn(gk::GraphLinkView{
                gk::GraphPinRef{ gk::GraphNodeRef{ it->second }, gk::EPinSide::OUTPUT, src_idx },
                gk::GraphPinRef{ gk::GraphNodeRef{ dst_index }, gk::EPinSide::INPUT, dst_pin } });
        };

        for (const ff::NodeStorage& storage : graph_->nodes())
        {
            const auto& ins = storage.node->inPins();
            for (std::uint32_t i = 0; i < ins.size(); ++i)
            {
                const ff::Pin* p = ins[i];
                if (p->kind() == ff::EPinKind::DATA_IN)
                {
                    if (const ff::DataOutPin* src =
                            static_cast<const ff::DataInPin*>(p)->linkedPin())
                    {
                        emit(src->node(), src, storage.index, i);
                    }
                }
                else if (p->kind() == ff::EPinKind::EXEC_IN)
                {
                    for (const ff::ExecOutPin* src :
                         static_cast<const ff::ExecInPin*>(p)->linkedPins())
                    {
                        if (src)
                        {
                            emit(src->node(), src, storage.index, i);
                        }
                    }
                }
            }
        }
    }

    std::optional<gk::GraphVec2> FlowGraphView::nodePos(gk::GraphNodeRef node) const
    {
        if (!graph_->hasNode(node.id))
        {
            return std::nullopt;
        }
        const auto* meta = static_cast<const FlowNodeEditorMeta*>(
            graph_->getNode(node.id).user_data.get());
        if (!meta || !meta->placed)
        {
            return std::nullopt;
        }
        return meta->pos;
    }

    void FlowGraphView::setNodePos(gk::GraphNodeRef node, gk::GraphVec2 pos)
    {
        if (!graph_->hasNode(node.id))
        {
            return;
        }
        ff::NodeStorage& storage = graph_->getNode(node.id);
        FlowNodeEditorMeta* meta = metaOf(storage);
        if (!meta)
        {
            storage.user_data = ff::NodeUserDataPtr(
                new FlowNodeEditorMeta{},
                [](void* p) { delete static_cast<FlowNodeEditorMeta*>(p); });
            meta = metaOf(storage);
        }
        meta->pos    = pos;
        meta->placed = true;
    }

    gk::GraphNodeRef FlowGraphView::addNode(std::string_view template_id)
    {
        ff::NodeCreatInfo* info = registry_->findNodeByName(std::string(template_id));
        if (!info || !info->creator)
        {
            return {};
        }
        std::unique_ptr<ff::Node> node = info->creator();
        if (!node)
        {
            return {};
        }
        return gk::GraphNodeRef{ graph_->addNodes(std::move(node)) };
    }

    gk::NodeCapture FlowGraphView::detachNode(gk::GraphNodeRef node)
    {
        ff::NodeStorage storage{ nullptr, 0, ff::NodeUserDataPtr(nullptr, [](void*) {}) };
        if (!graph_->extractNode(node.id, storage))
        {
            return nullptr;
        }
        return std::make_shared<ff::NodeStorage>(std::move(storage));
    }

    bool FlowGraphView::attachNode(gk::GraphNodeRef original, gk::NodeCapture capture)
    {
        const auto storage = std::static_pointer_cast<ff::NodeStorage>(capture);
        if (!storage || !storage->node)
        {
            return false;
        }
        // insertNodeAt checks occupancy BEFORE moving the arguments, so a
        // failed attach leaves the capture intact.
        return graph_->insertNodeAt(original.id, std::move(storage->node),
                                    std::move(storage->user_data));
    }

    bool FlowGraphView::connect(gk::GraphPinRef from, gk::GraphPinRef to)
    {
        ff::Pin* from_pin = pinAt(from); // OUTPUT side
        ff::Pin* to_pin   = pinAt(to);   // INPUT side
        if (!from_pin || !to_pin)
        {
            return false;
        }
        // The command stack pre-disconnects cap-1 endpoints, so linkTo's
        // replace-an-existing-link branch never fires here (LastLink stays
        // empty) — the no-implicit-severance contract holds.
        ff::LastLink last{};
        return to_pin->linkTo(from_pin, last) == ff::ELinkError::SUCCESS;
    }

    bool FlowGraphView::disconnect(gk::GraphPinRef from, gk::GraphPinRef to)
    {
        ff::Pin* from_pin = pinAt(from);
        ff::Pin* to_pin   = pinAt(to);
        if (!from_pin || !to_pin)
        {
            return false;
        }
        return to_pin->unlinkFrom(from_pin) == ff::ELinkError::SUCCESS;
    }

    void FlowGraphView::reconstructNode(gk::GraphNodeRef node)
    {
        if (graph_->hasNode(node.id))
        {
            graph_->getNode(node.id).node->reconstruct();
        }
    }

    // ======================== FlowSchema ====================================

    gk::ConnectResult FlowSchema::canConnect(gk::GraphPinRef from, gk::GraphPinRef to,
                                             const gk::IGraphView&) const
    {
        ff::Pin* from_pin = view_->pinAt(from);
        ff::Pin* to_pin   = view_->pinAt(to);
        if (!from_pin || !to_pin)
        {
            return gk::ConnectResult::no("stale pin");
        }
        // Pin::canLink is flowforge's connection-rule SSOT (kind matching,
        // reflection type compatibility, duplicate detection) — delegate.
        switch (to_pin->canLink(from_pin))
        {
        case ff::ELinkError::SUCCESS:
            return gk::ConnectResult::yes();
        case ff::ELinkError::SAME_NODE:
            return gk::ConnectResult::no("can't link a node to itself");
        case ff::ELinkError::WRONG_KIND:
            return gk::ConnectResult::no("incompatible pin kinds / types");
        case ff::ELinkError::HAS_LINKED:
            return gk::ConnectResult::no("already linked");
        default:
            return gk::ConnectResult::no("can't link");
        }
    }

    std::span<const gk::NodeTemplate> FlowSchema::palette() const
    {
        if (palette_cache_.empty())
        {
            for (const auto& info : registry_->node_creators())
            {
                palette_cache_.push_back(
                    gk::NodeTemplate{ info->name, info->name, info->category });
            }
        }
        return palette_cache_;
    }

    gk::PinStyleDesc FlowSchema::pinStyle(const gk::GraphPinType& type) const
    {
        if (type.category == kFlowExecCategory)
        {
            return gk::PinStyleDesc{ 0xFFFFFFFFu }; // exec: white
        }
        // Data pins: stable per-type color from the hashed category.
        static constexpr std::uint32_t kDataColors[8] = {
            0xFF7878E6u, // red-ish
            0xFF78DC78u, // green
            0xFFF0A078u, // blue
            0xFF5AC8F0u, // amber
            0xFFF078C8u, // violet
            0xFFDCDC5Au, // cyan
            0xFFC896F0u, // pink
            0xFFA0A0A0u, // grey
        };
        return gk::PinStyleDesc{ kDataColors[(type.category >> 4) & 7u] };
    }

    gk::NodeStyleDesc FlowSchema::nodeStyle(gk::GraphNodeRef node,
                                            const gk::IGraphView&) const
    {
        // Per-kind header tints (the FlowForge half of the shared blueprint
        // chrome): endpoints red, control flow slate, native calls blue,
        // object access green, everything else neutral.
        if (graph_->hasNode(node.id))
        {
            // No RTTI: every concrete node maps 1:1 to an ENodeOperation, so the
            // header tint keys off operation() instead of probing types.
            ff::Node*  n  = graph_->getNode(node.id).node.get();
            const auto op = n ? n->operation() : ff::ENodeOperation::INVALID;
            using EOp = ff::ENodeOperation;
            if (op == EOp::START || op == EOp::RETURN)
                return gk::NodeStyleDesc{ IM_COL32(180, 50, 60, 255) };
            if (op == EOp::BRANCH || op == EOp::SEQUENCE
                || op == EOp::FOR_LOOP || op == EOp::WHILE_LOOP)
                return gk::NodeStyleDesc{ IM_COL32(90, 110, 140, 255) };
            if (op == EOp::NATIVE_FUNC_CALL)
                return gk::NodeStyleDesc{ IM_COL32(40, 120, 200, 255) };
            if (op == EOp::GET_OBJECT || op == EOp::SET_OBJECT)
                return gk::NodeStyleDesc{ IM_COL32(60, 160, 110, 255) };
        }
        return gk::NodeStyleDesc{ IM_COL32(90, 90, 95, 255) };
    }

    std::span<const gk::NodeAction> FlowSchema::nodeActions(gk::GraphNodeRef node,
                                                            const gk::IGraphView&) const
    {
        actions_cache_.clear();
        if (graph_->hasNode(node.id))
        {
            ff::Node* raw = graph_->getNode(node.id).node.get();
            if (auto* seq = (raw && raw->operation() == ff::ENodeOperation::SEQUENCE)
                                ? static_cast<ff::SequenceNode*>(raw) : nullptr)
            {
                actions_cache_.push_back(gk::NodeAction{ "seq.add", "Add exec pin" });
                if (!seq->execOutPins().empty())
                {
                    actions_cache_.push_back(gk::NodeAction{ "seq.remove", "Remove exec pin" });
                }
            }
        }
        return actions_cache_;
    }

    void FlowSchema::runNodeAction(gk::GraphNodeRef node, std::string_view action_id,
                                   gk::IGraphView&)
    {
        if (!graph_->hasNode(node.id))
        {
            return;
        }
        ff::Node* raw = graph_->getNode(node.id).node.get();
        auto* seq = (raw && raw->operation() == ff::ENodeOperation::SEQUENCE)
                        ? static_cast<ff::SequenceNode*>(raw) : nullptr;
        if (!seq)
        {
            return;
        }
        if (action_id == "seq.add")
        {
            seq->addExecOutPin();
        }
        else if (action_id == "seq.remove" && !seq->execOutPins().empty())
        {
            seq->removeExecOutPin();
        }
    }

    void FlowSchema::drawNodeBody(gk::GraphNodeRef node, gk::IGraphView&,
                                  gk::DeferredPopupQueue&)
    {
        // Inline constant editors for UNCONNECTED data inputs (parity with the
        // retired NodePainter). ConstantEditor uses plain Drag/Input widgets,
        // which are canvas-safe; popup-shaped editors would go through the
        // deferred queue instead.
        if (!graph_->hasNode(node.id))
        {
            return;
        }
        ff::Node* n = graph_->getNode(node.id).node.get();
        for (ff::Pin* p : n->inPins())
        {
            if (p->kind() != ff::EPinKind::DATA_IN)
            {
                continue;
            }
            auto* in_pin = static_cast<ff::DataInPin*>(p);
            if (in_pin->linkedPin() || !in_pin->validConstant())
            {
                continue;
            }
            ImGui::PushID(in_pin);
            ImGui::TextUnformatted(p->name().c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ConstantEditor::editConstant("##const", in_pin->constantData());
            ImGui::PopID();
        }
    }

    // ======================== FlowGraphPanel ================================

    namespace
    {
        /// Hand-built reflected signature `void lux_flow_print(int value)` —
        /// RefFunction is an aggregate, no codegen needed. Function-local
        /// static: nodes borrow pointers into it for their whole lifetime.
        const lux::meta::RefFunction& printRefFunction()
        {
            static const lux::meta::RefFunction fn = []
            {
                lux::meta::RefFunction f{};
                f.invokable.name        = "lux_flow_print";
                f.invokable.full_name   = "lux_flow_print";
                f.invokable.return_type = lux::meta::ref_type_of_v<void>;
                f.invokable.parameters  = {
                    lux::meta::RefParam{ "value", lux::meta::ref_type_of_v<int> },
                };
                return f;
            }();
            return fn;
        }

        /// `void lux_debug_draw_line(9×float)` — the Milestone-D demo node.
        /// Scalar-only BY DESIGN (the MLIR lowering cannot lower records);
        /// the name matches the gameplay facade so the Run handler can bind
        /// the real function address under the same symbol.
        const lux::meta::RefFunction& drawLineRefFunction()
        {
            static const lux::meta::RefFunction fn = []
            {
                lux::meta::RefFunction f{};
                f.invokable.name        = "lux_debug_draw_line";
                f.invokable.full_name   = "lux_debug_draw_line";
                f.invokable.return_type = lux::meta::ref_type_of_v<void>;
                f.invokable.parameters  = {
                    lux::meta::RefParam{ "x0", lux::meta::ref_type_of_v<float> },
                    lux::meta::RefParam{ "y0", lux::meta::ref_type_of_v<float> },
                    lux::meta::RefParam{ "z0", lux::meta::ref_type_of_v<float> },
                    lux::meta::RefParam{ "x1", lux::meta::ref_type_of_v<float> },
                    lux::meta::RefParam{ "y1", lux::meta::ref_type_of_v<float> },
                    lux::meta::RefParam{ "z1", lux::meta::ref_type_of_v<float> },
                    lux::meta::RefParam{ "r", lux::meta::ref_type_of_v<float> },
                    lux::meta::RefParam{ "g", lux::meta::ref_type_of_v<float> },
                    lux::meta::RefParam{ "b", lux::meta::ref_type_of_v<float> },
                };
                return f;
            }();
            return fn;
        }

        /// `void lux_debug_draw_clear()` — zero-arg companion node.
        const lux::meta::RefFunction& drawClearRefFunction()
        {
            static const lux::meta::RefFunction fn = []
            {
                lux::meta::RefFunction f{};
                f.invokable.name        = "lux_debug_draw_clear";
                f.invokable.full_name   = "lux_debug_draw_clear";
                f.invokable.return_type = lux::meta::ref_type_of_v<void>;
                return f;
            }();
            return fn;
        }
    } // namespace

    FlowGraphPanel::FlowGraphPanel(std::string title)
        : Panel(std::move(title))
    {
        // Make Print creatable from the palette too (the builtin registry only
        // carries the control-flow nodes).
        {
            auto info      = std::make_unique<ff::NodeCreatInfo>();
            info->name     = "Print";
            info->category = "Native";
            info->creator  = []() -> std::unique_ptr<ff::Node>
            {
                return std::make_unique<ff::NativeFuncCall>(printRefFunction());
            };
            registry_.registerNode(std::move(info));
        }
        // Milestone-D demo node: draws a persistent line in the Scene
        // Viewport through the gameplay debug-draw facade. Constants are
        // pre-seeded (a green up-axis line) so the node runs out of the box;
        // every pin stays editable inline.
        {
            auto info      = std::make_unique<ff::NodeCreatInfo>();
            info->name     = "Draw Line";
            info->category = "Native";
            info->creator  = []() -> std::unique_ptr<ff::Node>
            {
                auto node = std::make_unique<ff::NativeFuncCall>(drawLineRefFunction());
                static constexpr float kDefaults[9] = {
                    0.0f, 0.0f, 0.0f,   // from
                    0.0f, 2.0f, 0.0f,   // to (up axis)
                    0.0f, 1.0f, 0.0f,   // green
                };
                const auto& pins = node->dataInPins();
                for (std::size_t i = 0; i < pins.size() && i < 9; ++i)
                {
                    pins[i]->setConstantData(lux::meta::RuntimeObject(kDefaults[i]));
                }
                return node;
            };
            registry_.registerNode(std::move(info));
        }
        {
            auto info      = std::make_unique<ff::NodeCreatInfo>();
            info->name     = "Clear Lines";
            info->category = "Native";
            info->creator  = []() -> std::unique_ptr<ff::Node>
            {
                return std::make_unique<ff::NativeFuncCall>(drawClearRefFunction());
            };
            registry_.registerNode(std::move(info));
        }

#if LUX_FLOWFORGE_HAS_MLIR
        ir_context_   = std::make_unique<ff::IRContext>();
        mlir_builder_ = std::make_unique<ff::MLIRBuilder>(ir_context_.get());
#endif

        buildHelloWorldGraph();
    }

    FlowGraphPanel::~FlowGraphPanel() = default;

    void FlowGraphPanel::buildHelloWorldGraph()
    {
        graph_ = ff::FlowGraph(); // fresh document (move-assign; address stable)

        auto start = std::make_unique<ff::StartNode>();
        auto print = std::make_unique<ff::NativeFuncCall>(printRefFunction());
        auto ret   = std::make_unique<ff::ReturnNode>();

        ff::LastLink last{};
        start->execOutPin().linkTo(&print->execInPin(), last);
        print->execOutPin().linkTo(&ret->execInPin(), last);
        if (!print->dataInPins().empty())
        {
            print->dataInPins()[0]->setConstantData(lux::meta::RuntimeObject(42));
        }

        const auto i_start = graph_.addNodes(std::move(start));
        const auto i_print = graph_.addNodes(std::move(print));
        const auto i_ret   = graph_.addNodes(std::move(ret));

        view_.setNodePos(gk::GraphNodeRef{ i_start }, gk::GraphVec2{ 60.0f, 140.0f });
        view_.setNodePos(gk::GraphNodeRef{ i_print }, gk::GraphVec2{ 320.0f, 140.0f });
        view_.setNodePos(gk::GraphNodeRef{ i_ret }, gk::GraphVec2{ 620.0f, 140.0f });

        editor_.bind(&view_, &schema_); // full reset (bimap, undo, selection)

#if LUX_FLOWFORGE_HAS_MLIR
        ir_buffer_.clear();
        run_status_.clear();
#endif
        g_flow_run_output.clear();
    }

    void FlowGraphPanel::paint()
    {
        auto& commands = editor_.commands();

        // ── toolbar ─────────────────────────────────────────────────────────
        if (ImGui::SmallButton("New"))
        {
            buildHelloWorldGraph(); // every new FlowGraph starts as Hello World
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!commands.canUndo());
        if (ImGui::SmallButton("Undo"))
        {
            editor_.undo(); // through the editor: resets id maps + re-syncs positions
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!commands.canRedo());
        if (ImGui::SmallButton("Redo"))
        {
            editor_.redo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Frame"))
        {
            editor_.navigateToContent();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu node(s) | right-click canvas to add",
                            graph_.nodes().size());

#if LUX_FLOWFORGE_HAS_MLIR
        ImGui::SameLine();
        if (ImGui::SmallButton("Generate IR"))
        {
            try
            {
                if (auto ir = mlir_builder_->generateIR(graph_))
                {
                    ir_buffer_ = ir->toString();
                }
            }
            catch (const std::exception& e)
            {
                ir_buffer_ = std::string("Error: ") + e.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Run"))
        {
            // Lower fresh (the IR module is mutated in place down to the LLVM
            // dialect by the JIT path, so each run gets its own IR; the pane
            // keeps the human-readable FlowForge dialect form).
            g_flow_run_output.clear();
            try
            {
                auto ir    = mlir_builder_->generateIR(graph_);
                ir_buffer_ = ir->toString();
                // Host symbols for the graph's NATIVE_FUNC_CALL externs —
                // names must match the RefFunction invokable names above.
                const int rc = ff::runMainJIT(
                    *ir,
                    { ff::JitNativeSymbol{ "lux_flow_print",
                                           reinterpret_cast<void*>(&lux_flow_print) },
                      ff::JitNativeSymbol{ "lux_debug_draw_line",
                                           reinterpret_cast<void*>(&lux_debug_draw_line) },
                      ff::JitNativeSymbol{ "lux_debug_draw_clear",
                                           reinterpret_cast<void*>(&lux_debug_draw_clear) } });
                run_status_ = "main() returned " + std::to_string(rc);
            }
            catch (const std::exception& e)
            {
                run_status_ = std::string("Run failed: ") + e.what();
            }
        }
        if (!ir_buffer_.empty() || !run_status_.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear"))
            {
                ir_buffer_.clear();
                run_status_.clear();
                g_flow_run_output.clear();
            }
        }

        if (!run_status_.empty())
        {
            ImGui::TextUnformatted(run_status_.c_str());
        }
        for (const std::string& line : g_flow_run_output)
        {
            ImGui::BulletText("%s", line.c_str());
        }
        if (!ir_buffer_.empty())
        {
            ImGui::InputTextMultiline(
                "##flow_ir", &ir_buffer_, ImVec2(-1.0f, 140.0f),
                ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
        }
#endif

        ImGui::Separator();

        // ── the shared canvas ───────────────────────────────────────────────
        editor_.paint("FlowGraphCanvas");

        // Flow raises no deferred popups yet; drain so requests never pile up.
        static_cast<void>(editor_.takeDeferredPopups());
    }

} // namespace lux::editor
