#include <lux/engine/editor/metadata/AssetEditorRegistration.hpp>
#include <lux/engine/editor/material/MaterialEditorImpl.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/HistoryCommands.hpp>
#include <algorithm>
#include <lux/engine/editor/material/MaterialPreview.hpp>
#include <cmath>
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

namespace lux::editor::material
{
    namespace
    {
        constexpr editing::HistoryLimits kLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};
        EditorFailure historyFailure(const editing::EditFailure& failure)
        {
            return {
                EEditorError::INVALID_STATE,
                "material.history",
                static_cast<std::uint64_t>(failure.code),
                {},
                failure
            };
        }

        struct NameAccess final
        {
            using Value = std::string;
            bool exists(const lux::material::MaterialSource&) const noexcept
            {
                return true;
            }
            Value read(const lux::material::MaterialSource& source) const
            {
                return source.name;
            }
            void exchange(lux::material::MaterialSource& source, Value& value) const noexcept
            {
                source.name.swap(value);
            }
            static bool equal(const Value& a, const Value& b) noexcept
            {
                return a == b;
            }
            static std::size_t bytes(const Value& value) noexcept
            {
                return value.capacity() + 1U;
            }
        };
        struct ConstantAccess final
        {
            using Value = std::array<float, 4>;
            lux::material::NodeId node;
            bool exists(const lux::material::MaterialSource& source) const noexcept
            {
                const auto* found = source.graph.node(node);
                return found && found->as<lux::material::ConstantNode>();
            }
            Value read(const lux::material::MaterialSource& source) const noexcept
            {
                Value value;
                std::ranges::copy(source.graph.node(node)->as<lux::material::ConstantNode>()->value, value.begin());
                return value;
            }
            void exchange(lux::material::MaterialSource& source, Value& value) const noexcept
            {
                auto* constant = source.graph.node(node)->as<lux::material::ConstantNode>();
                for (std::size_t i = 0; i < value.size(); ++i)
                {
                    std::swap(constant->value[i], value[i]);
                }
            }
            static bool equal(const Value& a, const Value& b) noexcept
            {
                return a == b;
            }
            static std::size_t bytes(const Value&) noexcept
            {
                return 0;
            }
        };
        struct ShadingAccess final
        {
            using Value = lux::rdesc::ELightingTechnique;
            bool exists(const lux::material::MaterialSource&) const noexcept
            {
                return true;
            }
            Value read(const lux::material::MaterialSource& source) const
            {
                return source.graph.shading_model;
            }
            void exchange(lux::material::MaterialSource& source, Value& value) const noexcept
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
            bool exists(const lux::material::MaterialSource&) const noexcept
            {
                return true;
            }
            Value read(const lux::material::MaterialSource& source) const noexcept
            {
                return source.graph.render_state;
            }
            void exchange(lux::material::MaterialSource& source, Value& value) const noexcept
            {
                std::swap(source.graph.render_state, value);
            }
            static bool equal(const Value& a, const Value& b) noexcept
            {
                return a.alpha_mode == b.alpha_mode && a.alpha_cutoff == b.alpha_cutoff &&
                       a.double_sided == b.double_sided;
            }
            static std::size_t bytes(const Value&) noexcept
            {
                return 0;
            }
        };
        template <auto Member> struct TSlotsAccess final
        {
            using Value = std::remove_cvref_t<decltype(std::declval<lux::material::MaterialGraph>().*Member)>;
            bool exists(const lux::material::MaterialSource&) const noexcept
            {
                return true;
            }
            Value read(const lux::material::MaterialSource& source) const
            {
                return source.graph.*Member;
            }
            void exchange(lux::material::MaterialSource& source, Value& value) const noexcept
            {
                (source.graph.*Member).swap(value);
            }
            static bool equal(const Value& first, const Value& second) noexcept
            {
                return first == second;
            }
            static std::size_t bytes(const Value& value) noexcept
            {
                auto total = value.capacity() * sizeof(typename Value::value_type);
                for (const auto& slot : value)
                {
                    total += slot.name.capacity() + 1;
                }
                return total;
            }
        };
        struct BusyGuard final
        {
            bool& busy_;
            explicit BusyGuard(bool& value) : busy_(value)
            {
                busy_ = true;
            }
            ~BusyGuard()
            {
                busy_ = false;
            }
        };
    } // namespace

    template <class Access> class MaterialEditor::Impl::TValueEdit final : public editing::EditOperation
    {
        using Value = typename Access::Value;
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(const TValueEdit& edit, Value value, bool changed)
                : edit_(edit), value_(std::move(value)), changed_(changed)
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

        private:
            void apply() noexcept override
            {
                edit_.access_.exchange(edit_.owner_.source_, value_);
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                lux::editor::detail::reportSignalDelivery(
                    edit_.owner_.editor_->emit(edit_.owner_.editor_->contentChanged, info.revision),
                    "MaterialEditor::contentChanged"
                );
            }
            const TValueEdit& edit_;
            Value value_;
            bool changed_;
        };

    public:
        TValueEdit(Impl& owner, Access access, Value after, std::string label, editing::StateId base)
            : owner_(owner), access_(access), before_(access.read(owner.source_)), after_(std::move(after)),
              label_(std::move(label)), base_(base)
        {}
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
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            const auto& expected = context.direction == editing::EDirection::FORWARD ? before_ : after_;
            const auto& next = context.direction == editing::EDirection::FORWARD ? after_ : before_;
            if (!access_.exists(owner_.source_) || !Access::equal(access_.read(owner_.source_), expected))
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
        Impl& owner_;
        Access access_;
        Value before_, after_;
        std::string label_;
        editing::StateId base_;
    };

    struct MaterialEditor::Impl::GraphDelta final
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
            for (const auto& node : insert)
            {
                // Built-in material nodes have only scalar payloads beyond their base storage.
                // Pin/name storage is charged independently, including capacity and terminators.
                constexpr auto object_bytes = std::max(
                    {sizeof(lux::material::ConstantNode),
                     sizeof(lux::material::InputNode),
                     sizeof(lux::material::SampleTextureNode),
                     sizeof(lux::material::ParamNode),
                     sizeof(lux::material::MathNode),
                     sizeof(lux::material::SwizzleNode),
                     sizeof(lux::material::ConstructNode),
                     sizeof(lux::material::DecodeNormalNode),
                     sizeof(lux::material::TbnTransformNode),
                     sizeof(lux::material::OutputSurfaceNode)}
                );
                bytes += object_bytes + node->name().capacity() + 1 +
                         (node->inputs().capacity() + node->outputs().capacity()) * sizeof(lux::material::DataPin);
                for (const auto& pin : node->inputs())
                {
                    bytes += pin.name.capacity() + 1;
                }
                for (const auto& pin : node->outputs())
                {
                    bytes += pin.name.capacity() + 1;
                }
            }
            return bytes;
        }
    };

    class MaterialEditor::Impl::GraphEditOperation final : public editing::EditOperation
    {
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(const GraphEditOperation& edit, lux::material::MaterialGraphEdit patch, bool initial, bool changed)
                : edit_(edit), patch_(std::move(patch)), initial_(initial), changed_(changed)
            {
                if (initial_)
                {
                    erased_ = edit.before_.erase;
                    placed_ = edit.after_.place;
                    std::size_t index{};
                    for (const auto* node : patch_.insertedNodes())
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
            void publish(const editing::CommitInfo& info) noexcept override
            {
                lux::editor::detail::reportSignalDelivery(
                    edit_.owner_.editor_->emit(edit_.owner_.editor_->contentChanged, info.revision),
                    "MaterialEditor::contentChanged"
                );
            }
            const GraphEditOperation& edit_;
            lux::material::MaterialGraphEdit patch_;
            bool initial_, changed_;
            std::vector<std::unique_ptr<lux::material::Node>> assigned_;
            std::vector<lux::material::NodeId> erased_;
            std::vector<lux::graph::GraphLayoutEntry> placed_;
        };

    public:
        GraphEditOperation(
            Impl& owner,
            GraphDelta before,
            GraphDelta after,
            std::string label,
            lux::graph::GraphNodeLayout placement = {}
        )
            : owner_(owner), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)),
              base_(owner.history_->view()->snapshot.current), placement_(placement)
        {}
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
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            const auto& delta = context.direction == editing::EDirection::FORWARD ? after_ : before_;
            const auto& topology = owner_.source_.graph.topology();
            const auto structural = topology.nodes().size() * sizeof(lux::graph::NodeRecord) +
                                    topology.pins().size() * sizeof(lux::graph::PinRecord) +
                                    topology.links().size() * sizeof(lux::graph::LinkRecord) +
                                    owner_.source_.graph.layout().all().size() * sizeof(lux::graph::GraphLayoutEntry);
            auto reserved = budget.reserve(sizeof(Plan) + 4U * (structural + delta.bytes() + 1024U));
            if (!reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            std::vector<const lux::material::Node*> inserted;
            inserted.reserve(delta.insert.size());
            for (const auto& node : delta.insert)
            {
                inserted.push_back(node.get());
            }
            auto patch = lux::material::MaterialGraphEdit::prepare(
                owner_.source_.graph,
                {inserted, delta.erase, delta.connect, delta.disconnect, delta.place, delta.unplace}
            );
            if (!patch)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    static_cast<std::uint64_t>(patch.error().code),
                    "Material graph transaction rejected"
                ));
            }
            const bool initial = context.kind == editing::EApplyKind::EXECUTE && !delta.insert.empty();
            if (initial)
            {
                std::size_t index{};
                for (const auto* node : patch->insertedNodes())
                {
                    if (delta.insert[index++]->id().valid())
                    {
                        continue;
                    }
                    auto placed = patch->place(node->id(), placement_);
                    if (!placed)
                    {
                        return lux::cxx::unexpected(editing::makeEditFailure(
                            editing::EEditError::PRECONDITION_FAILED,
                            static_cast<std::uint64_t>(placed.error().code)
                        ));
                    }
                }
            }
            return editing::PreparedEditPtr(new Plan(*this, std::move(*patch), initial, !delta.empty()));
        }

    private:
        Impl& owner_;
        mutable GraphDelta before_, after_; // Memento updates happen only in PreparedEdit::apply.
        std::string label_;
        editing::StateId base_;
        lux::graph::GraphNodeLayout placement_;
    };

    template <class Access>
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::change(
        Access access,
        typename Access::Value value,
        std::string label
    )
    {
        if (const auto admitted = canEdit(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (!access.exists(source_))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto view = history_->view();
        if (!view)
        {
            return lux::cxx::unexpected(view.error());
        }
        BusyGuard guard(busy_);
        editing::EditOperationPtr operation = std::make_unique<TValueEdit<Access>>(
            *this,
            access,
            std::move(value),
            std::move(label),
            view->snapshot.current
        );
        return history_->execute(operation);
    }

    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::editGraph(
        GraphDelta before,
        GraphDelta after,
        std::string label
    )
    {
        const auto admitted = canEdit();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }
        BusyGuard guard(busy_);
        editing::EditOperationPtr operation =
            std::make_unique<GraphEditOperation>(*this, std::move(before), std::move(after), std::move(label));
        return history_->execute(operation);
    }

    editing::EditResult<void> MaterialEditor::Impl::canEdit() const noexcept
    {
        if (!history_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));
        const bool is_changing = asset_status_.phase != EAssetEditPhase::IDLE || bool(saved_history_);
        const bool is_busy = busy_ || (is_changing && !finishing_interaction_);
        const bool is_read_only = !editor_context_.project().writable();
        if (is_busy || is_read_only)
            return lux::cxx::unexpected(
                editing::makeEditFailure(is_busy ? editing::EEditError::BUSY : editing::EEditError::BLOCKED_BY_HOST)
            );
        return {};
    }

    MaterialEditor::MaterialEditor(lux::ui::Root& parent, lux::ui::PaneId id, std::unique_ptr<Impl> data, EditorResult<void>& status)
        : lux::ui::Pane(
              parent,
              std::move(id),
              lux::ui::PaneTypeId{kMaterialEditorType},
              "Material Editor"
          ),
          impl_(std::move(data))
    {
        impl_->editor_ = this;
        impl_->createContent(status);
        impl_->close_connection_ = lux::editor::detail::takeConnection(
            lux::object::LuxObject::connect(
                this,
                &lux::ui::Pane::closeRequested,
                [this]() noexcept { impl_->hide_requested_ = true; }
            ),
            status
        );
    }
    MaterialEditor::~MaterialEditor() = default;

    EditorResult<std::unique_ptr<MaterialEditor>> MaterialEditor::create(
        lux::ui::Root& parent,
        lux::ui::PaneId id,
        EditorContext& context
    ) noexcept
    try
    {
        auto data = std::make_unique<Impl>(context);
        EditorResult<void> status;
        auto result = std::unique_ptr<MaterialEditor>(new MaterialEditor(parent, std::move(id), std::move(data), status));
        if (!status)
            return lux::cxx::unexpected(status.error());
        return result;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "material.create"});
    }

    const lux::material::MaterialSource& MaterialEditor::Impl::source() const noexcept
    {
        return this->source_;
    }
    ProjectStorage& MaterialEditor::Impl::project() noexcept
    {
        return editor_context_.project();
    }

    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::rename(std::string_view name)
    {
        if (name.empty() || name.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        const auto valid = lux::material::validateMaterialName(name);
        if (!valid)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::INVALID_ARGUMENT,
                static_cast<std::uint64_t>(valid.error().code)
            ));
        }
        return this->change(NameAccess{}, std::string(name), "Rename material");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setConstant(
        lux::material::NodeId node,
        const std::array<float, 4>& value
    )
    {
        if (!std::ranges::all_of(value, [](float v) { return std::isfinite(v); }))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        return this->change(ConstantAccess{node}, value, "Change constant");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setShadingModel(lux::rdesc::ELightingTechnique value
    )
    {
        if (value > lux::rdesc::ELightingTechnique::GRAPH)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        return this->change(ShadingAccess{}, value, "Change shading model");
    }

    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setRenderState(lux::material::RenderState state)
    {
        if (!std::isfinite(state.alpha_cutoff) || state.alpha_cutoff < 0 || state.alpha_cutoff > 1 ||
            static_cast<unsigned>(state.alpha_mode) > static_cast<unsigned>(lux::rdesc::EAlphaMode::BLEND))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        return this->change(RenderStateAccess{}, state, "Change render state");
    }

    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setTextureSlots(
        editing::StateId base,
        std::span<const lux::material::TextureSlotDecl> slots
    )
    {
        const auto admitted = this->canEdit();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }
        if (this->history_->view()->snapshot.current != base)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        if (slots.size() > lux::material::MaterialSourceLimits{}.max_slots)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::HISTORY_LIMIT));
        }
        const auto& previous = this->source_.graph.texture_slots;
        for (std::size_t index{}; index < slots.size(); ++index)
        {
            const auto asset = slots[index].texture;
            if (!asset.isNull() && (index >= previous.size() || asset != previous[index].texture))
            {
                const auto* entry = editor_context_.project().catalogAsset(asset);
                if (!entry || entry->magic != lux::asset::TextureAsset::primary_magic)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(
                        editing::EEditError::INVALID_ARGUMENT,
                        0,
                        "The selected asset is not a texture in this project"
                    ));
                }
            }
        }
        for (const auto& [id, node] : this->source_.graph.nodes())
        {
            const auto* sample = node->as<lux::material::SampleTextureNode>();
            if (sample && sample->texture_slot < previous.size() && sample->texture_slot >= slots.size())
            {
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    id.value,
                    "The removed texture slot is still referenced by a node"
                ));
            }
        }
        const auto valid = lux::material::validateMaterialTextureSlots(slots);
        if (!valid)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::INVALID_ARGUMENT,
                static_cast<std::uint64_t>(valid.error().code)
            ));
        }
        return this->change(
            TSlotsAccess<&lux::material::MaterialGraph::texture_slots>{},
            std::vector<lux::material::TextureSlotDecl>(slots.begin(), slots.end()),
            "Edit texture slots"
        );
    }

    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setParameterSlots(
        editing::StateId base,
        std::span<const lux::material::ParamSlotDecl> slots
    )
    {
        const auto admitted = this->canEdit();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }
        if (this->history_->view()->snapshot.current != base)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        if (slots.size() > lux::material::MaterialSourceLimits{}.max_slots)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::HISTORY_LIMIT));
        }
        for (const auto& [id, node] : this->source_.graph.nodes())
        {
            const auto* parameter = node->as<lux::material::ParamNode>();
            if (!parameter)
            {
                continue;
            }
            const bool removed =
                parameter->param_slot < this->source_.graph.param_slots.size() && parameter->param_slot >= slots.size();
            const bool type_changed =
                parameter->param_slot < slots.size() && parameter->type != slots[parameter->param_slot].type;
            if (removed || type_changed)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    id.value,
                    "Update parameter nodes before removing this slot or changing its type"
                ));
            }
        }
        const auto valid = lux::material::validateMaterialParameterSlots(slots);
        if (!valid)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::INVALID_ARGUMENT,
                static_cast<std::uint64_t>(valid.error().code)
            ));
        }
        return this->change(
            TSlotsAccess<&lux::material::MaterialGraph::param_slots>{},
            std::vector<lux::material::ParamSlotDecl>(slots.begin(), slots.end()),
            "Edit parameter slots"
        );
    }

    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::replaceNode(
        editing::StateId base,
        std::unique_ptr<lux::material::Node>& replacement
    )
    {
        const auto admitted = this->canEdit();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }
        if (this->history_->view()->snapshot.current != base)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        const auto* original = replacement ? this->source_.graph.node(replacement->id()) : nullptr;
        if (!original || original->kind() != replacement->kind())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        const auto same_pins = [](const auto& old_pins, const auto& new_pins) {
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
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::INVALID_ARGUMENT,
                0,
                "Node drafts must preserve existing pin identities"
            ));
        }
        const auto valid = lux::material::validateMaterialNode(*replacement);
        if (!valid)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::INVALID_ARGUMENT,
                static_cast<std::uint64_t>(valid.error().code)
            ));
        }
        Impl::GraphDelta before, after;
        if (!lux::material::equalMaterialNodes(*original, *replacement))
        {
            before.erase.push_back(original->id());
            after.erase.push_back(original->id());
            before.insert.push_back(original->clone());
            after.insert.push_back(replacement->clone());
            if (const auto* layout = this->source_.graph.layout().find(original->id()))
            {
                before.place.push_back({original->id(), *layout});
                after.place = before.place;
            }
            const auto& topology = this->source_.graph.topology();
            for (const auto& link : topology.links())
            {
                if (topology.findPin(link.from)->owner == original->id() ||
                    topology.findPin(link.to)->owner == original->id())
                {
                    before.connect.push_back(link);
                    after.connect.push_back(link);
                }
            }
        }
        auto result = this->editGraph(std::move(before), std::move(after), "Edit node properties");
        if (result)
        {
            replacement.reset();
        }
        return result;
    }

    editing::EditResult<lux::material::NodeId> MaterialEditor::Impl::insertNode(
        std::unique_ptr<lux::material::Node>& node,
        lux::graph::GraphNodeLayout placement
    )
    {
        auto admitted = this->canEdit();
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
        if (this->source_.graph.nodes().size() >= limits.max_nodes || pin_count > limits.max_pins ||
            this->source_.graph.topology().pins().size() > limits.max_pins - pin_count)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::HISTORY_LIMIT));
        }
        const auto valid = lux::material::validateMaterialNode(*node);
        if (!valid)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::INVALID_ARGUMENT,
                static_cast<std::uint64_t>(valid.error().code)
            ));
        }
        Impl::GraphDelta after;
        after.insert.push_back(node->clone());
        BusyGuard guard(this->busy_);
        auto change = std::make_unique<Impl::GraphEditOperation>(
            *this,
            Impl::GraphDelta{},
            std::move(after),
            "Add node",
            placement
        );
        auto* borrowed = change.get();
        editing::EditOperationPtr operation = std::move(change);
        auto result = this->history_->execute(operation);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        const auto id = borrowed->inserted();
        node.reset();
        return id;
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::removeNodes(
        std::span<const lux::material::NodeId> nodes,
        std::span<const lux::graph::LinkRecord> links
    )
    {
        auto admitted = this->canEdit();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }
        Impl::GraphDelta before, after;
        after.erase.assign(nodes.begin(), nodes.end());
        std::ranges::sort(after.erase);
        if (std::adjacent_find(after.erase.begin(), after.erase.end()) != after.erase.end())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        for (const auto id : after.erase)
        {
            const auto* node = this->source_.graph.node(id);
            if (!node)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            before.insert.push_back(node->clone());
            if (const auto* layout = this->source_.graph.layout().find(id))
            {
                before.place.push_back({id, *layout});
            }
        }
        for (const auto& link : this->source_.graph.topology().links())
        {
            const auto& topology = this->source_.graph.topology();
            const auto from = topology.findPin(link.from)->owner;
            const auto to = topology.findPin(link.to)->owner;
            if (std::ranges::binary_search(after.erase, from) || std::ranges::binary_search(after.erase, to))
            {
                before.connect.push_back(link);
            }
        }
        for (const auto& link : links)
        {
            if (std::ranges::find(before.connect, link) == before.connect.end())
            {
                before.connect.push_back(link);
                after.disconnect.push_back(link);
            }
        }
        return this->editGraph(std::move(before), std::move(after), "Remove nodes");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::connect(
        lux::material::PinId from,
        lux::material::PinId to
    )
    {
        Impl::GraphDelta before, after;
        const auto& topology = this->source_.graph.topology();
        if (!topology.findLink(from, to))
        {
            for (const auto& link : topology.links())
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
        return this->editGraph(std::move(before), std::move(after), "Connect pins");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::disconnect(
        lux::material::PinId from,
        lux::material::PinId to
    )
    {
        Impl::GraphDelta before, after;
        after.disconnect.push_back({from, to});
        before.connect.push_back({from, to});
        return this->editGraph(std::move(before), std::move(after), "Disconnect pins");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::moveNode(
        lux::material::NodeId node,
        lux::graph::GraphNodeLayout value
    )
    {
        const lux::graph::GraphLayoutEntry entry{node, value};
        return moveNodes(std::span{&entry, 1});
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::moveNodes(
        std::span<const lux::graph::GraphLayoutEntry> entries
    )
    {
        Impl::GraphDelta before, after;
        for (const auto& entry : entries)
        {
            const auto* current = this->source_.graph.layout().find(entry.node);
            if (!current || *current != entry.layout)
            {
                if (std::ranges::find(after.place, entry.node, &lux::graph::GraphLayoutEntry::node) !=
                    after.place.end())
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
        return this->editGraph(std::move(before), std::move(after), "Move nodes");
    }

    EditorResult<SaveRequestId> MaterialEditor::Impl::requestSave(std::string origin)
    {
        if (!history_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "material.empty"});
        if (asset_status_.phase != EAssetEditPhase::IDLE && asset_status_.phase != EAssetEditPhase::REVIEW)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save"});
        if (!editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "material.save"});
        }
        if (origin.empty() || !editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.save"});
        }
        if (this->busy_ || this->save_.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save"});
        }
        if (this->next_save_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "material.save"});
        }
        lux::material::MaterialSource capture{this->source_.id, this->source_.name, this->source_.graph.clone()};
        auto target = lux::editor::detail::captureAssetSaveTarget(editor_context_.project(), this->source_.id);
        if (!target)
            return lux::cxx::unexpected(target.error());
        auto ticket = this->history_->beginSave();
        if (!ticket)
        {
            return lux::cxx::unexpected(historyFailure(ticket.error()));
        }
        const SaveRequestId id{this->history_->id(), this->next_save_++};
        this->save_.emplace<MaterialSave>(
            id,
            *ticket,
            this->history_->view()->snapshot.revision,
            std::move(*target),
            std::move(capture),
            editor_context_.project(),
            editor_context_.execution(),
            *this->history_,
            completion_work_.requester()
        );
        return id;
    }
    std::span<const SaveRequestId> MaterialEditor::Impl::saveRequests() const noexcept
    {
        const auto* save_ = std::get_if<MaterialSave>(&this->save_);
        return save_ ? save_->requests() : std::span<const SaveRequestId>{};
    }
    EditorResult<VSaveRequestStatus> MaterialEditor::Impl::saveStatus(SaveRequestId id) const
    {
        const auto* save_ = std::get_if<MaterialSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
        }
        return save_->status();
    }
    EditorResult<void> MaterialEditor::Impl::retrySave(SaveRequestId id)
    {
        if (this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.retry"});
        }

        auto* save_ = std::get_if<MaterialSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
        }
        return save_->retry(true);
    }
    EditorResult<void> MaterialEditor::Impl::abandonSave(SaveRequestId id)
    {
        if (this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.abandonSave"});
        }

        auto* save_ = std::get_if<MaterialSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
        }
        save_->abandon();
        return {};
    }
    EditorResult<void> MaterialEditor::Impl::acknowledgeSave(SaveRequestId id)
    {
        if (this->busy_ || saved_history_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.acknowledgeSave"});
        }

        auto* save_ = std::get_if<MaterialSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
        }
        if (!save_->terminal())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save"});
        }
        this->save_.emplace<std::monostate>();
        return {};
    }

    EditorResult<lux::process::TaskId> MaterialEditor::Impl::requestCompile()
    {
        if (!history_ || saved_history_ || asset_status_.phase != EAssetEditPhase::IDLE)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.compile"});
        if (this->busy_ || compile_task_ ||
            compile_result_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.compile"});
        }
        auto view = this->history_->view();
        if (!view)
        {
            return lux::cxx::unexpected(historyFailure(view.error()));
        }
        auto assets = capturePreviewAssets();
        if (!assets)
            return lux::cxx::unexpected(assets.error());
        lux::material::MaterialSource capture{source_.id, source_.name, source_.graph.clone()};
        auto& execution = editor_context_.execution();
        auto admitted = execution.submit(
            {"Compile material", "compiler"},
            [source = std::move(capture),
             path = std::string(editor_context_.project().assetName(source_.id)),
             cpu = execution.cpu()](process::TaskReporter reporter) mutable noexcept {
                return compileMaterialAsset(std::move(source), std::move(path), cpu, reporter);
            },
            [this](process::TTaskResult<MaterialCompiled, EditorFailure>&& result) noexcept {
                compile_result_.emplace(detail::taskResult(std::move(result)));
                compile_task_ = {};
                completion_work_.request();
            }
        );
        if (!admitted)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "material.compile",
                static_cast<std::uint64_t>(admitted.error()),
                {},
                admitted.error()
            });
        const auto id = admitted->id();
        compilation_.emplace<Compilation>(
            id,
            history_->id(),
            view->snapshot.current,
            view->snapshot.revision,
            std::move(*assets)
        );
        compile_task_ = std::move(*admitted);
        return id;
    }
    EditorResult<SaveRequestId> MaterialEditor::Impl::requestPublish(lux::process::TaskId compile, std::string origin)
    {
        auto result = compiled(compile);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        if (!editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "material.publish"});
        }
        if (origin.empty() || !editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.publish"});
        }
        if (this->busy_ || this->save_.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.publish"});
        }
        if (this->next_save_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "material.publish"});
        }
        auto target = lux::editor::detail::captureAssetSaveTarget(editor_context_.project(), this->source_.id);
        if (!target)
            return lux::cxx::unexpected(target.error());
        auto ticket = this->history_->beginSave();
        if (!ticket)
        {
            return lux::cxx::unexpected(historyFailure(ticket.error()));
        }
        const SaveRequestId id{this->history_->id(), this->next_save_++};
        const auto& job = std::get<Compilation>(this->compilation_);
        const auto& image = job.output->publication;
        this->save_.emplace<MaterialSave>(
            id,
            *ticket,
            job.revision,
            std::move(*target),
            image,
            editor_context_.project(),
            editor_context_.execution(),
            *this->history_,
            completion_work_.requester()
        );
        return id;
    }

    EditorResult<VMaterialCompileStatus> MaterialEditor::Impl::compileStatus(lux::process::TaskId id) const
    {
        const auto* job = std::get_if<Compilation>(&this->compilation_);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.compile"});
        }
        if (const auto* success = std::get_if<MaterialCompileSucceeded>(&job->status))
        {
            auto value = *success;
            value.current = this->history_->view()->snapshot.current == value.captured;
            return VMaterialCompileStatus{value};
        }
        return job->status;
    }
    EditorResult<std::reference_wrapper<const lux::rdesc::MaterialDescription>> MaterialEditor::Impl::compiled(
        lux::process::TaskId id
    ) const
    {
        auto status = compileStatus(id);
        if (!status)
        {
            return lux::cxx::unexpected(status.error());
        }
        if (const auto* failed = std::get_if<MaterialCompileFailed>(&*status))
        {
            return lux::cxx::unexpected(failed->failure);
        }
        const auto* success = std::get_if<MaterialCompileSucceeded>(&*status);
        if (!success || !success->current)
        {
            return lux::cxx::unexpected(
                EditorFailure{success ? EEditorError::STALE_REQUEST : EEditorError::BUSY, "material.compile"}
            );
        }
        return std::cref(std::get<Compilation>(this->compilation_).output->artifact->data());
    }

    editing::HistoryId MaterialEditor::Impl::historyId() const noexcept
    {
        return history_ ? history_->id() : editing::HistoryId{};
    }
    editing::EditResult<editing::HistoryTargetView> MaterialEditor::Impl::historyView() const noexcept
    {
        if (!history_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));
        auto value = this->history_->view();
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        using Availability = editing::EHistoryActionAvailability;
        if (this->busy_ || saved_history_ || asset_status_.phase != EAssetEditPhase::IDLE)
        {
            return editing::HistoryTargetView{value->snapshot, Availability::BUSY, Availability::BUSY, {}, {}};
        }
        return editing::HistoryTargetView{
            value->snapshot,
            value->can_undo ? Availability::READY : Availability::EMPTY,
            value->can_redo ? Availability::READY : Availability::EMPTY,
            value->undo_label,
            value->redo_label
        };
    }
    editing::EditResult<editing::HistoryTargetResult> MaterialEditor::Impl::undo() noexcept
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(this->busy_);
        auto result = this->history_->undo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }
    editing::EditResult<editing::HistoryTargetResult> MaterialEditor::Impl::redo() noexcept
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(this->busy_);
        auto result = this->history_->redo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }
    void MaterialEditor::Impl::event(object::EventView& event) noexcept
    {
        if (auto* request = event.getIf<FinishEditingRequest>())
        {
            event.accept();
            request->result = finishEditing();
            return;
        }
        contentCommand(event);
        if (event.accepted())
            return;
        if (auto* query = event.getIf<AssetEditorQuery>())
        {
            event.accept();
            query->matches = !query->asset.isNull() &&
                (editor_->assetId() == query->asset || editor_->assetStatus().target == query->asset);
            return;
        }

        if (lux::editor::ui::receiveCloseRequest(*editor_, event, close_request_, close_prepared_, close_decision_))
            return;
        ui::dispatchHistoryCommand(*editor_, event);
    }

    EditorResult<void> MaterialEditor::Impl::finishEditing()
    {
        if (!history_)
            return {};
        if (this->busy_ || this->finishing_interaction_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.finish-editing"});
        BusyGuard finishing(this->finishing_interaction_);
        return finishContentEditing();
    }

    void MaterialEditor::Impl::adoptCompletions() noexcept
    {
        if (busy_)
            return;
        completion_deferred_ = false;
        const bool preview_busy = preview_ && preview_->scene &&
                                  !preview_->runtime.getSceneRegistry(*preview_->scene);
        if (preview_busy && std::holds_alternative<Compilation>(compilation_))
            completion_deferred_ = true;
        if (auto* job = std::get_if<Compilation>(&this->compilation_); job && compile_result_ && !preview_busy)
        {
            auto result = std::move(*compile_result_);
            compile_result_.reset();
            if (result)
            {
                if (history_ && asset_status_.phase == EAssetEditPhase::IDLE &&
                    job->history == this->history_->id() && this->history_->view()->snapshot.current == job->state)
                    updatePreview(result->publication.artifact.source_bytes, job->state, job->preview_assets);
                job->output.emplace(std::move(*result));
                job->status = MaterialCompileSucceeded{
                    job->state,
                    job->revision,
                    this->history_->view()->snapshot.current == job->state
                };
            }
            else
            {
                job->status = MaterialCompileFailed{job->state, job->revision, std::move(result.error())};
            }
            // Callbacks may request close; physical removal is an Editor safe-point operation.
            const auto completed_id = job->id;
            BusyGuard guard(this->busy_);
            lux::editor::detail::reportSignalDelivery(
                editor_->emit(editor_->compileFinished, completed_id),
                "compileFinished"
            );
        }
        adoptAssetResults();
        ui::reportCloseDecision(*editor_, close_request_, close_prepared_, close_decision_);
    }

    void MaterialEditor::Impl::update() noexcept
    {
        maintainPreview();
        if (this->busy_)
        {
            return;
        }
        const auto title = source_.id.isNull() ? std::string("Material Editor") : source_.name;
        if (editor_->title() != title)
            editor_->setTitle(title);
        if (std::exchange(hide_requested_, false))
        {
            PaneCloseRequest request{editor_};
            static_cast<void>(object::routeEvent(*editor_, editor_->root(), request));
        }
        if (history_ &&
            asset_status_.phase == EAssetEditPhase::IDLE && !saved_history_)
            applyContentIntents();
        const bool has_pending_change = completion_deferred_ || asset_status_.phase != EAssetEditPhase::IDLE;
        if (has_pending_change)
            editor_->root().deferChange(*editor_, [](object::LuxObject& target) noexcept {
                static_cast<MaterialEditor&>(target).impl_->applyChanges();
            });
    }

} // namespace lux::editor::material

namespace lux::editor::material
{
    EditorResult<SaveRequestId> MaterialEditor::requestSave(std::string origin)
    {
        return impl_->requestSave(std::move(origin));
    }

    std::span<const SaveRequestId> MaterialEditor::saveRequests() const noexcept
    {
        return impl_->saveRequests();
    }

    EditorResult<VSaveRequestStatus> MaterialEditor::saveStatus(SaveRequestId id) const
    {
        return impl_->saveStatus(std::move(id));
    }

    EditorResult<void> MaterialEditor::retrySave(SaveRequestId id)
    {
        return impl_->retrySave(std::move(id));
    }

    EditorResult<void> MaterialEditor::abandonSave(SaveRequestId id)
    {
        return impl_->abandonSave(std::move(id));
    }

    EditorResult<void> MaterialEditor::acknowledgeSave(SaveRequestId id)
    {
        return impl_->acknowledgeSave(std::move(id));
    }

    EditorResult<lux::process::TaskId> MaterialEditor::requestCompile()
    {
        return impl_->requestCompile();
    }

    EditorResult<SaveRequestId> MaterialEditor::requestPublish(lux::process::TaskId compile, std::string origin)
    {
        return impl_->requestPublish(std::move(compile), std::move(origin));
    }

    EditorResult<VMaterialCompileStatus> MaterialEditor::compileStatus(lux::process::TaskId id) const
    {
        return impl_->compileStatus(std::move(id));
    }

    EditorResult<std::reference_wrapper<const lux::rdesc::MaterialDescription>> MaterialEditor::compiled(
        lux::process::TaskId id
    ) const
    {
        return impl_->compiled(std::move(id));
    }

    editing::HistoryId MaterialEditor::historyId() const noexcept
    {
        return impl_->historyId();
    }

    editing::EditResult<editing::HistoryTargetView> MaterialEditor::historyView() const noexcept
    {
        return impl_->historyView();
    }

    editing::EditResult<editing::HistoryTargetResult> MaterialEditor::undo() noexcept
    {
        return impl_->undo();
    }

    editing::EditResult<editing::HistoryTargetResult> MaterialEditor::redo() noexcept
    {
        return impl_->redo();
    }

    void MaterialEditor::event(object::EventView& event) noexcept
    {
        return impl_->event(event);
    }

    EditorResult<void> MaterialEditor::finishEditing()
    {
        return impl_->finishEditing();
    }

    void MaterialEditor::update() noexcept
    {
        impl_->update();
        lux::editor::ui::reportCloseDecision(
            *this,
            impl_->close_request_,
            impl_->close_prepared_,
            impl_->close_decision_
        );
    }
}

namespace lux::editor::material
{
    void MaterialEditor::Impl::applyChanges() noexcept
    {
        if (busy_)
            return;
        adoptCompletions();
        applyAssetChange();
    }
}
