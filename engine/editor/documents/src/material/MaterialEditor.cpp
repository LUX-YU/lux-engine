#include <algorithm>
#include <cmath>
#include <lux/engine/editor/DocumentSave.hpp>
#include <lux/engine/editor/DocumentSource.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

namespace lux::editor::material
{
namespace
{
constexpr editing::HistoryLimits kLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};
EditorFailure historyFailure(const editing::EditFailure &failure)
{
    return {EEditorError::INVALID_STATE, "material.history", static_cast<std::uint64_t>(failure.code), {}, failure};
}
struct MaterialEncoder final
{
    EditorResult<lux::cxx::SharedBytes<>> operator()(const lux::material::MaterialSourceDocument &capture,
                                                     std::stop_token stop) const noexcept
    {
        if (stop.stop_requested())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "material.encode"});
        }
        auto result = lux::material::encodeMaterialSource(capture);
        if (!result)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "material.encode",
                                                      static_cast<std::uint64_t>(result.error().code),
                                                      result.error().field, result.error()});
        }
        auto owner = std::make_shared<const std::string>(std::move(*result));
        return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span{owner->data(), owner->size()}));
    }
};
using MaterialSave = detail::DocumentSave<lux::material::MaterialSourceDocument, MaterialEncoder>;

struct NameAccess final
{
    using Value = std::string;
    bool exists(const lux::material::MaterialSourceDocument &) const noexcept
    {
        return true;
    }
    Value read(const lux::material::MaterialSourceDocument &source) const
    {
        return source.name;
    }
    void exchange(lux::material::MaterialSourceDocument &source, Value &value) const noexcept
    {
        source.name.swap(value);
    }
    static bool equal(const Value &a, const Value &b) noexcept
    {
        return a == b;
    }
    static std::size_t bytes(const Value &value) noexcept
    {
        return value.capacity() + 1U;
    }
};
struct ConstantAccess final
{
    using Value = std::array<float, 4>;
    lux::material::NodeId node;
    bool exists(const lux::material::MaterialSourceDocument &source) const noexcept
    {
        const auto *found = source.graph.node(node);
        return found && found->as<lux::material::ConstantNode>();
    }
    Value read(const lux::material::MaterialSourceDocument &source) const noexcept
    {
        Value value;
        std::ranges::copy(source.graph.node(node)->as<lux::material::ConstantNode>()->value, value.begin());
        return value;
    }
    void exchange(lux::material::MaterialSourceDocument &source, Value &value) const noexcept
    {
        auto *constant = source.graph.node(node)->as<lux::material::ConstantNode>();
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            std::swap(constant->value[i], value[i]);
        }
    }
    static bool equal(const Value &a, const Value &b) noexcept
    {
        return a == b;
    }
    static std::size_t bytes(const Value &) noexcept
    {
        return 0;
    }
};
struct ShadingAccess final
{
    using Value = lux::rdesc::ELightingTechnique;
    bool exists(const lux::material::MaterialSourceDocument &) const noexcept
    {
        return true;
    }
    Value read(const lux::material::MaterialSourceDocument &source) const
    {
        return source.graph.shading_model;
    }
    void exchange(lux::material::MaterialSourceDocument &source, Value &value) const noexcept
    {
        std::swap(source.graph.shading_model, value);
    }
    static bool equal(Value a, Value b) noexcept
    {
        return a == b;
    }
    static std::size_t bytes(Value) noexcept
    {
        return sizeof(Value);
    }
};

struct RenderStateAccess final
{
    using Value = lux::material::RenderState;
    bool exists(const lux::material::MaterialSourceDocument &) const noexcept
    {
        return true;
    }
    Value read(const lux::material::MaterialSourceDocument &source) const noexcept
    {
        return source.graph.render_state;
    }
    void exchange(lux::material::MaterialSourceDocument &source, Value &value) const noexcept
    {
        std::swap(source.graph.render_state, value);
    }
    static bool equal(const Value &a, const Value &b) noexcept
    {
        return a.alpha_mode == b.alpha_mode && a.alpha_cutoff == b.alpha_cutoff && a.double_sided == b.double_sided;
    }
    static std::size_t bytes(const Value &) noexcept
    {
        return 0;
    }
};
template <auto Member> struct SlotsAccess final
{
    using Value = std::remove_cvref_t<decltype(std::declval<lux::material::MaterialGraph>().*Member)>;
    bool exists(const lux::material::MaterialSourceDocument &) const noexcept
    {
        return true;
    }
    Value read(const lux::material::MaterialSourceDocument &source) const
    {
        return source.graph.*Member;
    }
    void exchange(lux::material::MaterialSourceDocument &source, Value &value) const noexcept
    {
        (source.graph.*Member).swap(value);
    }
    static bool equal(const Value &first, const Value &second) noexcept
    {
        return first == second;
    }
    static std::size_t bytes(const Value &value) noexcept
    {
        auto total = value.capacity() * sizeof(typename Value::value_type);
        for (const auto &slot : value)
        {
            total += slot.name.capacity() + 1;
        }
        return total;
    }
};
struct BusyGuard final
{
    bool &busy;
    explicit BusyGuard(bool &value) : busy(value)
    {
        busy = true;
    }
    ~BusyGuard()
    {
        busy = false;
    }
};
} // namespace

struct MaterialEditor::Data final
{
    template <class Access> class ValueEdit final : public editing::EditOperation
    {
        using Value = typename Access::Value;
        class Plan final : public editing::PreparedEdit
        {
          public:
            Plan(const ValueEdit &edit, Value value, bool changed)
                : edit_(edit), value_(std::move(value)), changed_(changed)
            {
            }
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

          private:
            void apply() noexcept override
            {
                edit_.access_.exchange(edit_.owner_.source, value_);
            }
            void publish(const editing::CommitInfo &info) noexcept override
            {
                edit_.owner_.editor->notify<MaterialEditor::contentChanged>(info.revision);
            }
            const ValueEdit &edit_;
            Value value_;
            bool changed_;
        };

      public:
        ValueEdit(Data &owner, Access access, Value after, std::string label, editing::StateId base)
            : owner_(owner), access_(access), before_(access.read(owner.source)), after_(std::move(after)),
              label_(std::move(label)), base_(base)
        {
        }
        editing::HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return label_;
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(*this) + label_.capacity() + 1U + Access::bytes(before_) + Access::bytes(after_);
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
        {
            const auto &expected = context.direction == editing::EDirection::FORWARD ? before_ : after_;
            const auto &next = context.direction == editing::EDirection::FORWARD ? after_ : before_;
            if (!access_.exists(owner_.source) || !Access::equal(access_.read(owner_.source), expected))
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            auto reserved = budget.reserve(sizeof(Plan) + Access::bytes(next));
            if (!reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            return editing::PreparedEditPtr(new Plan(*this, next, !Access::equal(before_, after_)));
        }

      private:
        Data &owner_;
        Access access_;
        Value before_, after_;
        std::string label_;
        editing::StateId base_;
    };

    struct GraphDelta final
    {
        std::vector<std::unique_ptr<lux::material::Node>> insert;
        std::vector<lux::material::NodeId> erase;
        std::vector<lux::graph::LinkRecord> connect, disconnect;
        std::vector<lux::graph::GraphLayoutEntry> place;
        std::vector<lux::material::NodeId> unplace;
        bool empty() const noexcept
        {
            return insert.empty() && erase.empty() && connect.empty() && disconnect.empty() && place.empty() &&
                   unplace.empty();
        }
        std::size_t bytes() const noexcept
        {
            auto bytes = insert.capacity() * sizeof(std::unique_ptr<lux::material::Node>) +
                         (erase.capacity() + unplace.capacity()) * sizeof(lux::material::NodeId) +
                         (connect.capacity() + disconnect.capacity()) * sizeof(lux::graph::LinkRecord) +
                         place.capacity() * sizeof(lux::graph::GraphLayoutEntry);
            for (const auto &node : insert)
            {
                // Built-in material nodes have only scalar payloads beyond their base storage.
                // Pin/name storage is charged independently, including capacity and terminators.
                constexpr auto object_bytes =
                    std::max({sizeof(lux::material::ConstantNode), sizeof(lux::material::InputNode),
                              sizeof(lux::material::SampleTextureNode), sizeof(lux::material::ParamNode),
                              sizeof(lux::material::MathNode), sizeof(lux::material::SwizzleNode),
                              sizeof(lux::material::ConstructNode), sizeof(lux::material::DecodeNormalNode),
                              sizeof(lux::material::TbnTransformNode), sizeof(lux::material::OutputSurfaceNode)});
                bytes += object_bytes + node->name().capacity() + 1 +
                         (node->inputs().capacity() + node->outputs().capacity()) * sizeof(lux::material::DataPin);
                for (const auto &pin : node->inputs())
                {
                    bytes += pin.name.capacity() + 1;
                }
                for (const auto &pin : node->outputs())
                {
                    bytes += pin.name.capacity() + 1;
                }
            }
            return bytes;
        }
    };
    class GraphEdit final : public editing::EditOperation
    {
        class Plan final : public editing::PreparedEdit
        {
          public:
            Plan(const GraphEdit &edit, lux::material::MaterialGraphEdit patch, bool initial, bool changed)
                : edit_(edit), patch_(std::move(patch)), initial_(initial), changed_(changed)
            {
                if (initial_)
                {
                    erased_ = edit.before_.erase;
                    placed_ = edit.after_.place;
                    std::size_t index{};
                    for (const auto *node : patch_.insertedNodes())
                    {
                        assigned_.push_back(node->clone());
                        if (!edit.after_.insert[index]->id().valid())
                        {
                            erased_.push_back(node->id());
                            placed_.push_back({node->id(), edit.placement_});
                        }
                        ++index;
                    }
                }
            }
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

          private:
            void apply() noexcept override
            {
                patch_.commit();
                if (initial_)
                {
                    edit_.after_.insert.swap(assigned_);
                    edit_.after_.place.swap(placed_);
                    edit_.before_.erase.swap(erased_);
                }
            }
            void publish(const editing::CommitInfo &info) noexcept override
            {
                edit_.owner_.editor->notify<MaterialEditor::contentChanged>(info.revision);
            }
            const GraphEdit &edit_;
            lux::material::MaterialGraphEdit patch_;
            bool initial_, changed_;
            std::vector<std::unique_ptr<lux::material::Node>> assigned_;
            std::vector<lux::material::NodeId> erased_;
            std::vector<lux::graph::GraphLayoutEntry> placed_;
        };

      public:
        GraphEdit(Data &owner, GraphDelta before, GraphDelta after, std::string label,
                  lux::graph::GraphNodeLayout placement = {})
            : owner_(owner), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)),
              base_(owner.history->view()->snapshot.current), placement_(placement)
        {
        }
        editing::HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return label_;
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            // Fresh IDs add a layout and inverse erase record, not another whole graph.
            return sizeof(*this) + before_.bytes() + after_.bytes() + label_.capacity() + 1U +
                   after_.insert.size() * (sizeof(lux::graph::GraphLayoutEntry) + sizeof(lux::material::NodeId));
        }
        lux::material::NodeId inserted() const noexcept
        {
            return after_.insert.front()->id();
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
        {
            const auto &delta = context.direction == editing::EDirection::FORWARD ? after_ : before_;
            const auto &topology = owner_.source.graph.topology();
            const auto structural = topology.nodes().size() * sizeof(lux::graph::NodeRecord) +
                                    topology.pins().size() * sizeof(lux::graph::PinRecord) +
                                    topology.links().size() * sizeof(lux::graph::LinkRecord) +
                                    owner_.source.graph.layout().all().size() * sizeof(lux::graph::GraphLayoutEntry);
            auto reserved = budget.reserve(sizeof(Plan) + 4U * (structural + delta.bytes() + 1024U));
            if (!reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            std::vector<const lux::material::Node *> inserted;
            inserted.reserve(delta.insert.size());
            for (const auto &node : delta.insert)
            {
                inserted.push_back(node.get());
            }
            auto patch = lux::material::MaterialGraphEdit::prepare(
                owner_.source.graph,
                {inserted, delta.erase, delta.connect, delta.disconnect, delta.place, delta.unplace});
            if (!patch)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                                     static_cast<std::uint64_t>(patch.error().code),
                                                                     "Material graph transaction rejected"));
            }
            const bool initial = context.kind == editing::EApplyKind::EXECUTE && !delta.insert.empty();
            if (initial)
            {
                std::size_t index{};
                for (const auto *node : patch->insertedNodes())
                {
                    if (delta.insert[index++]->id().valid())
                    {
                        continue;
                    }
                    auto placed = patch->place(node->id(), placement_);
                    if (!placed)
                    {
                        return lux::cxx::unexpected(editing::makeEditFailure(
                            editing::EEditError::PRECONDITION_FAILED, static_cast<std::uint64_t>(placed.error().code)));
                    }
                }
            }
            return editing::PreparedEditPtr(new Plan(*this, std::move(*patch), initial, !delta.empty()));
        }

      private:
        Data &owner_;
        mutable GraphDelta before_, after_; // Memento updates happen only in PreparedEdit::apply.
        std::string label_;
        editing::StateId base_;
        lux::graph::GraphNodeLayout placement_;
    };

    editing::EditResult<void> canEdit() const noexcept
    {
        if (close != ECloseState::OPEN || close_requested)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
        }
        if (busy || !project.writable())
        {
            return lux::cxx::unexpected(
                editing::makeEditFailure(busy ? editing::EEditError::BUSY : editing::EEditError::BLOCKED_BY_HOST));
        }
        return {};
    }
    editing::EditResult<editing::ApplyResult> editGraph(GraphDelta before, GraphDelta after, std::string label)
    {
        const auto admitted = canEdit();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }
        BusyGuard guard(busy);
        editing::EditOperationPtr operation =
            std::make_unique<GraphEdit>(*this, std::move(before), std::move(after), std::move(label));
        return history->execute(operation);
    }

    using Compiled = detail::CompiledDocument<asset::MaterialAsset>;
    struct CompileWork final
    {
        const lux::material::MaterialSourceDocument *source;
        std::string path;
        std::stop_token stop;
        EditorResult<Compiled> operator()() const noexcept
        {
            if (stop.stop_requested())
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "material.compile"});
            }
            auto result = lux::material::compileMaterial(source->graph);
            if (!result)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "material.compile",
                                                          static_cast<std::uint64_t>(result.error().code),
                                                          result.error().message, std::move(result.error())});
            }
            auto source_bytes = MaterialEncoder{}(*source, stop);
            if (!source_bytes)
            {
                return lux::cxx::unexpected(source_bytes.error());
            }
            auto material = std::make_shared<const lux::rdesc::MaterialDescription>(std::move(*result));
            auto asset =
                asset::MaterialAsset::create({source->id, asset::MaterialAsset::asset_type}, std::move(material));
            if (!asset)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                          "material.artifact",
                                                          static_cast<std::uint64_t>(asset.error().code),
                                                          {},
                                                          asset.error()});
            }
            return detail::encodeCompiledDocument(std::move(*asset), std::move(*source_bytes), path);
        }
    };
    struct Compilation final
    {
        Compilation(MaterialCompileId request, const editing::HistorySnapshot &history,
                    const lux::material::MaterialSourceDocument &source, std::string path,
                    process::ExecutionRuntime &runtime)
            : id(request), state(history.current), revision(history.revision),
              capture{source.id, source.name, source.graph.clone()},
              task(runtime, stdexec::then(stdexec::schedule(runtime.cpu()),
                                          CompileWork{&capture, std::move(path), stop.get_token()}))
        {
            task.start();
        }
        MaterialCompileId id;
        editing::StateId state;
        editing::Revision revision;
        lux::material::MaterialSourceDocument capture;
        std::stop_source stop;
        detail::ScheduledDocumentTask<process::CpuScheduler, CompileWork> task;
        MaterialCompileStatus status{MaterialCompilePending{}};
        std::variant<std::monostate, Compiled> output;
    };

    Data(lux::material::MaterialSourceDocument value, Project &project_owner, process::ExecutionRuntime &execution,
         std::unique_ptr<editing::EditHistory> edits)
        : source(std::move(value)), project(project_owner), runtime(execution), history(std::move(edits))
    {
    }

    template <class Access>
    editing::EditResult<editing::ApplyResult> change(Access access, typename Access::Value value, std::string label)
    {
        if (close != ECloseState::OPEN || close_requested)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
        }
        if (busy || !project.writable())
        {
            return lux::cxx::unexpected(
                editing::makeEditFailure(busy ? editing::EEditError::BUSY : editing::EEditError::BLOCKED_BY_HOST));
        }
        if (!access.exists(source))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto view = history->view();
        if (!view)
        {
            return lux::cxx::unexpected(view.error());
        }
        BusyGuard guard(busy);
        editing::EditOperationPtr operation = std::make_unique<ValueEdit<Access>>(
            *this, access, std::move(value), std::move(label), view->snapshot.current);
        return history->execute(operation);
    }

    MaterialEditor *editor{}; // Bound once, before publishing the complete document.
    lux::material::MaterialSourceDocument source;
    Project &project;
    process::ExecutionRuntime &runtime;
    std::unique_ptr<editing::EditHistory> history;
    std::vector<std::unique_ptr<DocumentView>> views;
    std::variant<std::monostate, MaterialSave> save;
    std::variant<std::monostate, Compilation> compilation;
    std::uint64_t next_save{1}, next_compile{1};
    ECloseState close{ECloseState::OPEN};
    bool close_requested{}, busy{};
};

MaterialEditor::MaterialEditor(object::ObjectDispatcherRef dispatcher, std::unique_ptr<Data> data)
    : Object(dispatcher), data_(std::move(data))
{
    data_->editor = this;
}
MaterialEditor::~MaterialEditor() = default;

EditorResult<std::unique_ptr<MaterialEditor>> MaterialEditor::open(lux::material::MaterialSourceDocument &source,
                                                                   Project &project, process::ExecutionRuntime &runtime)
{
    const auto *asset = project.asset(source.id);
    if (!asset || asset->kind != EProjectAssetKind::MATERIAL_GRAPH)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.identity"});
    }
    auto valid = lux::material::validateMaterialSource(source);
    if (!valid)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "material.source",
                                                  static_cast<std::uint64_t>(valid.error().code), valid.error().field,
                                                  valid.error()});
    }
    auto history = editing::EditHistory::create({kLimits, {}, true});
    if (!history)
    {
        return lux::cxx::unexpected(historyFailure(history.error()));
    }
    auto data = std::make_unique<Data>(std::move(source), project, runtime, std::move(*history));
    return std::unique_ptr<MaterialEditor>(new MaterialEditor(project.dispatcherRef(), std::move(data)));
}

DocumentSummary MaterialEditor::summary() const
{
    return {handle(),
            {data_->project.manifest().id, data_->source.id, std::string(kMaterialDocumentType)},
            data_->source.name,
            !data_->project.writable()};
}
const lux::material::MaterialSourceDocument &MaterialEditor::source() const noexcept
{
    return data_->source;
}
Project &MaterialEditor::project() noexcept
{
    return data_->project;
}

editing::EditResult<editing::ApplyResult> MaterialEditor::rename(std::string_view name)
{
    if (name.empty() || name.size() > 4096)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
    }
    lux::material::MaterialSourceDocument candidate{data_->source.id, std::string(name), {}};
    const auto valid = lux::material::validateMaterialSource(candidate);
    if (!valid)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT,
                                                             static_cast<std::uint64_t>(valid.error().code)));
    }
    return data_->change(NameAccess{}, std::move(candidate.name), "Rename material");
}
editing::EditResult<editing::ApplyResult> MaterialEditor::setConstant(lux::material::NodeId node,
                                                                      const std::array<float, 4> &value)
{
    if (!std::ranges::all_of(value, [](float v) { return std::isfinite(v); }))
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
    }
    return data_->change(ConstantAccess{node}, value, "Change constant");
}
editing::EditResult<editing::ApplyResult> MaterialEditor::setShadingModel(lux::rdesc::ELightingTechnique value)
{
    if (value > lux::rdesc::ELightingTechnique::Graph)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
    }
    return data_->change(ShadingAccess{}, value, "Change shading model");
}

editing::EditResult<editing::ApplyResult> MaterialEditor::setRenderState(lux::material::RenderState state)
{
    if (!std::isfinite(state.alpha_cutoff) || state.alpha_cutoff < 0 || state.alpha_cutoff > 1 ||
        static_cast<unsigned>(state.alpha_mode) > static_cast<unsigned>(lux::rdesc::EAlphaMode::Blend))
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
    }
    return data_->change(RenderStateAccess{}, state, "Change render state");
}

editing::EditResult<editing::ApplyResult> MaterialEditor::setTextureSlots(
    editing::StateId base, std::span<const lux::material::TextureSlotDecl> slots)
{
    const auto admitted = data_->canEdit();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }
    if (data_->history->view()->snapshot.current != base)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
    }
    if (slots.size() > lux::material::MaterialSourceLimits{}.max_slots)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::HISTORY_LIMIT));
    }
    const auto &previous = data_->source.graph.texture_slots;
    for (std::size_t index{}; index < slots.size(); ++index)
    {
        const auto asset = slots[index].texture;
        if (!asset.isNull() && (index >= previous.size() || asset != previous[index].texture))
        {
            const auto *entry = data_->project.catalogAsset(asset);
            if (!entry || entry->magic != lux::asset::TextureAsset::primary_magic)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::INVALID_ARGUMENT, 0, "The selected asset is not a texture in this project"));
            }
        }
    }
    for (const auto &[id, node] : data_->source.graph.nodes())
    {
        const auto *sample = node->as<lux::material::SampleTextureNode>();
        if (sample && sample->texture_slot < previous.size() && sample->texture_slot >= slots.size())
        {
            return lux::cxx::unexpected(
                editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, id.value,
                                         "The removed texture slot is still referenced by a node"));
        }
    }
    lux::material::MaterialSourceDocument candidate{data_->source.id, "Slots", {}};
    candidate.graph.texture_slots.assign(slots.begin(), slots.end());
    const auto valid = lux::material::validateMaterialSource(candidate);
    if (!valid)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT,
                                                             static_cast<std::uint64_t>(valid.error().code)));
    }
    return data_->change(SlotsAccess<&lux::material::MaterialGraph::texture_slots>{},
                         std::move(candidate.graph.texture_slots), "Edit texture slots");
}

editing::EditResult<editing::ApplyResult> MaterialEditor::setParameterSlots(
    editing::StateId base, std::span<const lux::material::ParamSlotDecl> slots)
{
    const auto admitted = data_->canEdit();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }
    if (data_->history->view()->snapshot.current != base)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
    }
    if (slots.size() > lux::material::MaterialSourceLimits{}.max_slots)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::HISTORY_LIMIT));
    }
    for (const auto &[id, node] : data_->source.graph.nodes())
    {
        const auto *parameter = node->as<lux::material::ParamNode>();
        if (!parameter)
        {
            continue;
        }
        const bool removed =
            parameter->param_slot < data_->source.graph.param_slots.size() && parameter->param_slot >= slots.size();
        const bool type_changed =
            parameter->param_slot < slots.size() && parameter->type != slots[parameter->param_slot].type;
        if (removed || type_changed)
        {
            return lux::cxx::unexpected(
                editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, id.value,
                                         "Update parameter nodes before removing this slot or changing its type"));
        }
    }
    lux::material::MaterialSourceDocument candidate{data_->source.id, "Slots", {}};
    candidate.graph.param_slots.assign(slots.begin(), slots.end());
    const auto valid = lux::material::validateMaterialSource(candidate);
    if (!valid)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT,
                                                             static_cast<std::uint64_t>(valid.error().code)));
    }
    return data_->change(SlotsAccess<&lux::material::MaterialGraph::param_slots>{},
                         std::move(candidate.graph.param_slots), "Edit parameter slots");
}

editing::EditResult<editing::ApplyResult> MaterialEditor::replaceNode(editing::StateId base,
                                                                      std::unique_ptr<lux::material::Node> &replacement)
{
    const auto admitted = data_->canEdit();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }
    if (data_->history->view()->snapshot.current != base)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
    }
    const auto *original = replacement ? data_->source.graph.node(replacement->id()) : nullptr;
    if (!original || original->kind() != replacement->kind())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
    }
    const auto same_pins = [](const auto &old_pins, const auto &new_pins) {
        for (std::size_t index{}; index < new_pins.size(); ++index)
        {
            if (index < old_pins.size() ? old_pins[index].id != new_pins[index].id : new_pins[index].id.valid())
            {
                return false;
            }
        }
        return true;
    };
    if (!same_pins(original->inputs(), replacement->inputs()) ||
        !same_pins(original->outputs(), replacement->outputs()))
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT, 0,
                                                             "Node drafts must preserve existing pin identities"));
    }
    lux::material::MaterialSourceDocument candidate{data_->source.id, "Node", {}};
    const auto assigned = candidate.graph.addNodeWithId(replacement->id(), replacement->clone());
    const auto valid = lux::material::validateMaterialSource(candidate);
    if (!assigned.valid() || !valid)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(
            editing::EEditError::INVALID_ARGUMENT, valid ? 0 : static_cast<std::uint64_t>(valid.error().code)));
    }
    Data::GraphDelta before, after;
    if (!lux::material::equalMaterialNodes(*original, *replacement))
    {
        before.erase.push_back(original->id());
        after.erase.push_back(original->id());
        before.insert.push_back(original->clone());
        after.insert.push_back(replacement->clone());
        if (const auto *layout = data_->source.graph.layout().find(original->id()))
        {
            before.place.push_back({original->id(), *layout});
            after.place = before.place;
        }
        const auto &topology = data_->source.graph.topology();
        for (const auto &link : topology.links())
        {
            if (topology.findPin(link.from)->owner == original->id() ||
                topology.findPin(link.to)->owner == original->id())
            {
                before.connect.push_back(link);
                after.connect.push_back(link);
            }
        }
    }
    auto result = data_->editGraph(std::move(before), std::move(after), "Edit node properties");
    if (result)
    {
        replacement.reset();
    }
    return result;
}

editing::EditResult<lux::material::NodeId> MaterialEditor::insertNode(std::unique_ptr<lux::material::Node> &node,
                                                                      lux::graph::GraphNodeLayout placement)
{
    auto admitted = data_->canEdit();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }
    if (!node || node->id().valid() || !std::isfinite(placement.x) || !std::isfinite(placement.y))
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
    }
    const auto limits = lux::material::MaterialSourceLimits{};
    const auto pin_count = node->inputs().size() + node->outputs().size();
    if (data_->source.graph.nodes().size() >= limits.max_nodes || pin_count > limits.max_pins ||
        data_->source.graph.topology().pins().size() > limits.max_pins - pin_count)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::HISTORY_LIMIT));
    }
    lux::material::MaterialSourceDocument candidate{data_->source.id, "Node", {}};
    const auto assigned = candidate.graph.addNode(node->clone());
    auto valid = lux::material::validateMaterialSource(candidate);
    if (!assigned.valid() || !valid)
    {
        return lux::cxx::unexpected(editing::makeEditFailure(
            editing::EEditError::INVALID_ARGUMENT, valid ? 0 : static_cast<std::uint64_t>(valid.error().code)));
    }
    Data::GraphDelta after;
    after.insert.push_back(node->clone());
    BusyGuard guard(data_->busy);
    auto change =
        std::make_unique<Data::GraphEdit>(*data_, Data::GraphDelta{}, std::move(after), "Add node", placement);
    auto *borrowed = change.get();
    editing::EditOperationPtr operation = std::move(change);
    auto result = data_->history->execute(operation);
    if (!result)
    {
        return lux::cxx::unexpected(result.error());
    }
    const auto id = borrowed->inserted();
    node.reset();
    return id;
}
editing::EditResult<editing::ApplyResult> MaterialEditor::removeNodes(std::span<const lux::material::NodeId> nodes,
                                                                      std::span<const lux::graph::LinkRecord> links)
{
    auto admitted = data_->canEdit();
    if (!admitted)
    {
        return lux::cxx::unexpected(admitted.error());
    }
    Data::GraphDelta before, after;
    after.erase.assign(nodes.begin(), nodes.end());
    std::ranges::sort(after.erase);
    if (std::adjacent_find(after.erase.begin(), after.erase.end()) != after.erase.end())
    {
        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
    }
    for (const auto id : after.erase)
    {
        const auto *node = data_->source.graph.node(id);
        if (!node)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        before.insert.push_back(node->clone());
        if (const auto *layout = data_->source.graph.layout().find(id))
        {
            before.place.push_back({id, *layout});
        }
    }
    for (const auto &link : data_->source.graph.topology().links())
    {
        const auto &topology = data_->source.graph.topology();
        const auto from = topology.findPin(link.from)->owner;
        const auto to = topology.findPin(link.to)->owner;
        if (std::ranges::binary_search(after.erase, from) || std::ranges::binary_search(after.erase, to))
        {
            before.connect.push_back(link);
        }
    }
    for (const auto &link : links)
    {
        if (std::ranges::find(before.connect, link) == before.connect.end())
        {
            before.connect.push_back(link);
            after.disconnect.push_back(link);
        }
    }
    return data_->editGraph(std::move(before), std::move(after), "Remove nodes");
}
editing::EditResult<editing::ApplyResult> MaterialEditor::connect(lux::material::PinId from, lux::material::PinId to)
{
    Data::GraphDelta before, after;
    const auto &topology = data_->source.graph.topology();
    if (!topology.findLink(from, to))
    {
        for (const auto &link : topology.links())
        {
            if (link.to == to)
            {
                after.disconnect.push_back(link);
                before.connect.push_back(link);
            }
        }
        after.connect.push_back({from, to});
        before.disconnect.push_back({from, to});
    }
    return data_->editGraph(std::move(before), std::move(after), "Connect pins");
}
editing::EditResult<editing::ApplyResult> MaterialEditor::disconnect(lux::material::PinId from, lux::material::PinId to)
{
    Data::GraphDelta before, after;
    after.disconnect.push_back({from, to});
    before.connect.push_back({from, to});
    return data_->editGraph(std::move(before), std::move(after), "Disconnect pins");
}
editing::EditResult<editing::ApplyResult> MaterialEditor::moveNode(lux::material::NodeId node,
                                                                   lux::graph::GraphNodeLayout value)
{
    const lux::graph::GraphLayoutEntry entry{node, value};
    return moveNodes(std::span{&entry, 1});
}
editing::EditResult<editing::ApplyResult> MaterialEditor::moveNodes(
    std::span<const lux::graph::GraphLayoutEntry> entries)
{
    Data::GraphDelta before, after;
    for (const auto &entry : entries)
    {
        const auto *current = data_->source.graph.layout().find(entry.node);
        if (!current || *current != entry.layout)
        {
            if (std::ranges::find(after.place, entry.node, &lux::graph::GraphLayoutEntry::node) != after.place.end())
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
            }
            after.place.push_back(entry);
            if (current)
            {
                before.place.push_back({entry.node, *current});
            }
            else
            {
                before.unplace.push_back(entry.node);
            }
        }
    }
    return data_->editGraph(std::move(before), std::move(after), "Move nodes");
}

EditorResult<SaveRequestId> MaterialEditor::requestSave(std::string origin)
{
    if (!data_->project.writable())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "material.save"});
    }
    if (origin.empty() || !data_->runtime.blocking())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.save"});
    }
    if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy || data_->save.index() != 0)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save"});
    }
    if (data_->next_save == UINT64_MAX)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "material.save"});
    }
    lux::material::MaterialSourceDocument capture{data_->source.id, data_->source.name, data_->source.graph.clone()};
    auto ticket = data_->history->beginSave();
    if (!ticket)
    {
        return lux::cxx::unexpected(historyFailure(ticket.error()));
    }
    const SaveRequestId id{handle(), data_->next_save++};
    data_->save.emplace<MaterialSave>(id, *ticket, data_->history->view()->snapshot.revision, data_->source.id,
                                      std::move(capture), data_->project, data_->runtime, *data_->history);
    return id;
}
std::span<const SaveRequestId> MaterialEditor::saveRequests() const noexcept
{
    const auto *save = std::get_if<MaterialSave>(&data_->save);
    return save ? save->requests() : std::span<const SaveRequestId>{};
}
EditorResult<SaveRequestStatus> MaterialEditor::saveStatus(SaveRequestId id) const
{
    const auto *save = std::get_if<MaterialSave>(&data_->save);
    if (!save || save->id() != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
    }
    return save->status();
}
EditorResult<void> MaterialEditor::retrySave(SaveRequestId id)
{
    auto *save = std::get_if<MaterialSave>(&data_->save);
    if (!save || save->id() != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
    }
    return save->retry();
}
EditorResult<void> MaterialEditor::abandonSave(SaveRequestId id)
{
    auto *save = std::get_if<MaterialSave>(&data_->save);
    if (!save || save->id() != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
    }
    save->abandon();
    return {};
}
EditorResult<void> MaterialEditor::acknowledgeSave(SaveRequestId id)
{
    auto *save = std::get_if<MaterialSave>(&data_->save);
    if (!save || save->id() != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
    }
    if (!save->terminal())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save"});
    }
    data_->save.emplace<std::monostate>();
    return {};
}

EditorResult<MaterialCompileId> MaterialEditor::requestCompile()
{
    if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy || data_->compilation.index() != 0)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.compile"});
    }
    if (data_->next_compile == UINT64_MAX)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "material.compile"});
    }
    auto view = data_->history->view();
    if (!view)
    {
        return lux::cxx::unexpected(historyFailure(view.error()));
    }
    const MaterialCompileId id{handle(), data_->next_compile++};
    data_->compilation.emplace<Data::Compilation>(
        id, view->snapshot, data_->source, std::string(data_->project.assetName(data_->source.id)), data_->runtime);
    return id;
}
EditorResult<SaveRequestId> MaterialEditor::requestPublish(MaterialCompileId compile, std::string origin)
{
    auto result = compiled(compile);
    if (!result)
    {
        return lux::cxx::unexpected(result.error());
    }
    if (!data_->project.writable())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "material.publish"});
    }
    if (origin.empty() || !data_->runtime.blocking())
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.publish"});
    }
    if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy || data_->save.index() != 0)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.publish"});
    }
    if (data_->next_save == UINT64_MAX)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "material.publish"});
    }
    auto ticket = data_->history->beginSave();
    if (!ticket)
    {
        return lux::cxx::unexpected(historyFailure(ticket.error()));
    }
    const SaveRequestId id{handle(), data_->next_save++};
    const auto &job = std::get<Data::Compilation>(data_->compilation);
    const auto &image = std::get<Data::Compiled>(job.output).image;
    data_->save.emplace<MaterialSave>(id, *ticket, job.revision, data_->source.id, image, data_->project,
                                      data_->runtime, *data_->history);
    return id;
}

EditorResult<MaterialCompileStatus> MaterialEditor::compileStatus(MaterialCompileId id) const
{
    const auto *job = std::get_if<Data::Compilation>(&data_->compilation);
    if (!job || job->id != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.compile"});
    }
    if (const auto *success = std::get_if<MaterialCompileSucceeded>(&job->status))
    {
        auto value = *success;
        value.current = data_->history->view()->snapshot.current == value.captured;
        return MaterialCompileStatus{value};
    }
    return job->status;
}
EditorResult<std::reference_wrapper<const lux::rdesc::MaterialDescription>> MaterialEditor::compiled(
    MaterialCompileId id) const
{
    auto status = compileStatus(id);
    if (!status)
    {
        return lux::cxx::unexpected(status.error());
    }
    if (const auto *failed = std::get_if<MaterialCompileFailed>(&*status))
    {
        return lux::cxx::unexpected(failed->failure);
    }
    const auto *success = std::get_if<MaterialCompileSucceeded>(&*status);
    if (!success || !success->current)
    {
        return lux::cxx::unexpected(
            EditorFailure{success ? EEditorError::STALE_REQUEST : EEditorError::BUSY, "material.compile"});
    }
    return std::cref(std::get<Data::Compiled>(std::get<Data::Compilation>(data_->compilation).output).artifact->data());
}
EditorResult<void> MaterialEditor::acknowledgeCompile(MaterialCompileId id)
{
    auto *job = std::get_if<Data::Compilation>(&data_->compilation);
    if (!job || job->id != id)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.compile"});
    }
    if (std::holds_alternative<MaterialCompilePending>(job->status))
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.compile"});
    }
    data_->compilation.emplace<std::monostate>();
    return {};
}

EditorResult<void> MaterialEditor::addViews(std::vector<std::unique_ptr<DocumentView>> &views)
{
    if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "material.views"});
    }
    for (const auto &view : views)
    {
        if (!view || view->closeStatus().state != ECloseState::OPEN)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.views"});
        }
    }
    data_->views.reserve(data_->views.size() + views.size());
    for (auto &view : views)
    {
        data_->views.push_back(std::move(view));
    }
    views.clear();
    return {};
}
std::span<const std::unique_ptr<DocumentView>> MaterialEditor::views() const noexcept
{
    return data_->views;
}
editing::HistoryId MaterialEditor::historyId() const noexcept
{
    return data_->history->id();
}
editing::EditResult<editing::HistoryTargetView> MaterialEditor::historyView() const noexcept
{
    auto value = data_->history->view();
    if (!value)
    {
        return lux::cxx::unexpected(value.error());
    }
    using Availability = editing::EHistoryActionAvailability;
    if (data_->close != ECloseState::OPEN || data_->close_requested)
    {
        return editing::HistoryTargetView{value->snapshot, Availability::CLOSED, Availability::CLOSED, {}, {}};
    }
    if (data_->busy)
    {
        return editing::HistoryTargetView{value->snapshot, Availability::BUSY, Availability::BUSY, {}, {}};
    }
    return editing::HistoryTargetView{value->snapshot, value->can_undo ? Availability::READY : Availability::EMPTY,
                                      value->can_redo ? Availability::READY : Availability::EMPTY, value->undo_label,
                                      value->redo_label};
}
editing::EditResult<editing::HistoryTargetResult> MaterialEditor::undo() noexcept
{
    if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy)
    {
        return lux::cxx::unexpected(
            editing::makeEditFailure(data_->busy ? editing::EEditError::BUSY : editing::EEditError::CLOSED));
    }
    BusyGuard guard(data_->busy);
    auto result = data_->history->undo();
    if (!result)
    {
        return lux::cxx::unexpected(result.error());
    }
    return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
}
editing::EditResult<editing::HistoryTargetResult> MaterialEditor::redo() noexcept
{
    if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy)
    {
        return lux::cxx::unexpected(
            editing::makeEditFailure(data_->busy ? editing::EEditError::BUSY : editing::EEditError::CLOSED));
    }
    BusyGuard guard(data_->busy);
    auto result = data_->history->redo();
    if (!result)
    {
        return lux::cxx::unexpected(result.error());
    }
    return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
}
void MaterialEditor::requestClose() noexcept
{
    data_->close_requested = true;
    if (data_->busy || data_->close != ECloseState::OPEN)
    {
        return;
    }
    if (auto *save = std::get_if<MaterialSave>(&data_->save); save && !save->terminal())
    {
        save->abandon();
    }
    if (auto *job = std::get_if<Data::Compilation>(&data_->compilation))
    {
        job->stop.request_stop();
    }
    for (const auto &view : data_->views)
    {
        view->requestClose();
    }
    data_->close = ECloseState::CLOSING;
}
CloseStatus MaterialEditor::closeStatus() const
{
    if (data_->close_requested && data_->close != ECloseState::CLOSED)
    {
        if (const auto *save = std::get_if<MaterialSave>(&data_->save))
        {
            if (const auto *failed = std::get_if<SaveRetryable>(&save->status()))
            {
                return {ECloseState::CLOSING, "Resolve the retained material save",
                        lux::cxx::unexpected(failed->failure)};
            }
        }
        return {ECloseState::CLOSING, "Material views, save and compilation"};
    }
    return {data_->close, {}};
}
void MaterialEditor::poll(PollBudget &budget)
{
    if (data_->busy)
    {
        return;
    }
    if (auto *save = std::get_if<MaterialSave>(&data_->save))
    {
        save->poll();
    }
    if (auto *job = std::get_if<Data::Compilation>(&data_->compilation);
        job && job->task.ready() && std::holds_alternative<MaterialCompilePending>(job->status))
    {
        auto result = job->task.take();
        if (result)
        {
            job->output.emplace<Data::Compiled>(std::move(*result));
            job->status = MaterialCompileSucceeded{job->state, job->revision,
                                                   data_->history->view()->snapshot.current == job->state};
        }
        else
        {
            job->status = MaterialCompileFailed{job->state, job->revision, std::move(result.error())};
        }
        // Callbacks may request close; physical removal is an Editor safe-point operation.
        BusyGuard guard(data_->busy);
        notify<compileFinished>(job->id);
    }
    if (data_->close_requested)
    {
        requestClose();
    }
    for (const auto &view : data_->views)
    {
        view->poll(budget);
    }
    std::erase_if(data_->views, [](const auto &view) { return view->closeStatus().state == ECloseState::CLOSED; });
    if (data_->close != ECloseState::CLOSING || !data_->views.empty())
    {
        return;
    }
    if (const auto *save = std::get_if<MaterialSave>(&data_->save); save && !save->terminal())
    {
        return;
    }
    if (const auto *job = std::get_if<Data::Compilation>(&data_->compilation); job && !job->task.ready())
    {
        return;
    }
    data_->compilation.emplace<std::monostate>();
    data_->save.emplace<std::monostate>();
    if (data_->history->close())
    {
        data_->close = ECloseState::CLOSED;
    }
}
namespace
{
struct MaterialCodec final
{
    using Source = lux::material::MaterialSourceDocument;
    static constexpr std::size_t max_bytes = 16U * 1024U * 1024U;
    static lux::asset::AssetId identity(const Source &source) noexcept
    {
        return source.id;
    }
    static EditorResult<Source> decode(const lux::cxx::SharedBytes<> &bytes, std::stop_token) noexcept
    {
        auto source = lux::material::decodeMaterialSource({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        if (!source)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "material.decode",
                                                      static_cast<std::uint64_t>(source.error().code),
                                                      source.error().field, source.error()});
        }
        return std::move(*source);
    }
    static EditorResult<std::unique_ptr<DocumentEditor>> adopt(Source &source, Project &project,
                                                               process::ExecutionRuntime &runtime)
    {
        auto result = MaterialEditor::open(source, project, runtime);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return std::unique_ptr<DocumentEditor>(std::move(*result));
    }
};
} // namespace
EditorResult<std::unique_ptr<DocumentOpening>> openMaterialDocument(Project &project,
                                                                    const OpenDocumentRequest &request,
                                                                    process::ExecutionRuntime &runtime)
{
    const auto *entry = project.asset(request.key.source);
    if (!entry || entry->kind != EProjectAssetKind::MATERIAL_GRAPH || !runtime.blocking() ||
        request.key.project != project.manifest().id || request.key.type != kMaterialDocumentType)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.open"});
    }
    return std::unique_ptr<DocumentOpening>(new detail::SourceOpening<MaterialCodec>(project, *entry, runtime));
}

} // namespace lux::editor::material
