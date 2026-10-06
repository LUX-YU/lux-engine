#include "MaterialEditPreparation.hpp"

namespace lux::editor::material::detail
{
    bool MaterialGraphDelta::empty() const noexcept
    {
        return insert.empty() && erase.empty() && connect.empty() && disconnect.empty() && place.empty() &&
               unplace.empty();
    }
    std::size_t MaterialGraphDelta::bytes() const noexcept
    {
        std::size_t bytes = insert.capacity() * sizeof(std::unique_ptr<lux::material::Node>) +
                            (erase.capacity() + unplace.capacity()) * sizeof(lux::material::NodeId) +
                            (connect.capacity() + disconnect.capacity()) * sizeof(lux::graph::LinkRecord) +
                            place.capacity() * sizeof(lux::graph::GraphLayoutEntry);
        for (const auto& node : insert)
            bytes += nodeBytes(*node);
        return bytes;
    }
    namespace
    {
        MaterialEditResult<void> applyPatch(
            lux::material::MaterialGraph& graph,
            lux::material::MaterialGraphChange change
        )
        {
            auto patch = lux::material::MaterialGraphEdit::prepare(graph, change);
            if (!patch)
            {
                MaterialEditError error{EMaterialEditError::INVALID_GRAPH, patch.error().node};
                error.graph = patch.error();
                return lux::cxx::unexpected(error);
            }
            patch->commit();
            return {};
        }
        bool preservesPins(const auto& before, const auto& after)
        {
            for (std::size_t i{}; i < after.size(); ++i)
                if (i < before.size() ? before[i].id != after[i].id : after[i].id.valid())
                    return false;
            return true;
        }
    }
    MaterialEditResult<void> applyGraphEdit(
        lux::material::MaterialSource& source,
        VMaterialEdit& edit,
        std::vector<lux::material::NodeId>& inserted
    )
    {
        return std::visit(
            [&](auto& item) -> MaterialEditResult<void> {
                using T = std::decay_t<decltype(item)>;
                auto& graph = source.graph;
                if constexpr (std::same_as<T, MaterialInsertNode>)
                {
                    const bool invalid = !item.code.valid() || !item.value || !std::isfinite(item.placement.x) ||
                                         !std::isfinite(item.placement.y);
                    if (invalid)
                        return rejected(EMaterialEditError::INVALID_NODE);
                    if (!lux::material::validateMaterialNode(*item.value))
                        return rejected(EMaterialEditError::INVALID_VALUE);
                    const auto id = item.value->id();
                    const bool duplicate = id.valid() && graph.node(id);
                    if (duplicate)
                        return rejected(EMaterialEditError::INVALID_NODE, id);
                    auto copy = item.value->clone();
                    const auto assigned =
                        id.valid() ? graph.addNodeWithId(id, std::move(copy)) : graph.addNode(std::move(copy));
                    if (!assigned.valid())
                        return rejected(EMaterialEditError::INVALID_GRAPH, id);
                    auto placed = graph.layout().set(assigned, item.placement);
                    if (!placed)
                        return rejected(EMaterialEditError::INVALID_GRAPH, assigned);
                    inserted.push_back(assigned);
                }
                else if constexpr (std::same_as<T, MaterialReplaceNode>)
                {
                    const auto* before = item.value ? graph.node(item.value->id()) : nullptr;
                    const bool invalid =
                        !item.code.valid() || !before || (before && before->kind() != item.value->kind());
                    if (invalid)
                        return rejected(EMaterialEditError::INVALID_NODE);
                    const bool invalid_pins = !preservesPins(before->inputs(), item.value->inputs()) ||
                                              !preservesPins(before->outputs(), item.value->outputs());
                    if (invalid_pins || !lux::material::validateMaterialNode(*item.value))
                        return rejected(EMaterialEditError::INVALID_VALUE, before->id());
                    const auto id = before->id();
                    std::vector<lux::graph::LinkRecord> links;
                    for (const auto& link : graph.topology().links())
                        if (graph.topology().findPin(link.from)->owner == id ||
                            graph.topology().findPin(link.to)->owner == id)
                            links.push_back(link);
                    std::vector<lux::graph::GraphLayoutEntry> place;
                    if (const auto* current = graph.layout().find(id))
                        place.push_back({id, *current});
                    const lux::material::Node* next = item.value.get();
                    return applyPatch(graph, {std::span{&next, 1}, std::span{&id, 1}, links, {}, place, {}});
                }
                else if constexpr (std::same_as<T, MaterialEraseNode>)
                {
                    if (!graph.node(item.node))
                        return rejected(EMaterialEditError::INVALID_NODE, item.node);
                    graph.removeNode(item.node);
                }
                else if constexpr (std::same_as<T, MaterialConnect>)
                {
                    if (graph.topology().findLink(item.from, item.to))
                        return {};
                    const lux::graph::LinkRecord link{item.from, item.to};
                    const auto previous = graph.topology().incoming(item.to);
                    const auto removed =
                        previous ? std::span{&*previous, 1} : std::span<const lux::graph::LinkRecord>{};
                    return applyPatch(graph, {{}, {}, std::span{&link, 1}, removed, {}, {}});
                }
                else if constexpr (std::same_as<T, MaterialDisconnect>)
                {
                    const lux::graph::LinkRecord link{item.from, item.to};
                    return applyPatch(graph, {{}, {}, {}, std::span{&link, 1}, {}, {}});
                }
                else if constexpr (std::same_as<T, MaterialPlaceNode>)
                {
                    const bool invalid =
                        !graph.node(item.node) || !std::isfinite(item.value.x) || !std::isfinite(item.value.y);
                    if (invalid)
                        return rejected(EMaterialEditError::INVALID_VALUE, item.node);
                    if (!graph.layout().set(item.node, item.value))
                        return rejected(EMaterialEditError::INVALID_GRAPH);
                }
                else
                {
                    MaterialValueEdit value;
                    auto checked = value.set(source, VMaterialValue{item});
                    if (!checked)
                        return checked;
                    swapValue(source, value.after.front());
                }
                return {};
            },
            edit
        );
    }
    MaterialEditResult<void> graphDifference(
        const lux::material::MaterialGraph& source,
        const lux::material::MaterialGraph& candidate,
        MaterialGraphDelta& before,
        MaterialGraphDelta& after
    )
    {
        for (const auto& [id, node] : source.nodes())
        {
            const auto* next = candidate.node(id);
            if (next && lux::material::equalMaterialNodes(*node, *next))
                continue;
            auto copy = node->clone();
            if (!copy || !lux::material::equalMaterialNodes(*node, *copy))
                return rejected(EMaterialEditError::CALLBACK, id);
            before.insert.push_back(std::move(copy));
            after.erase.push_back(id);
        }
        for (const auto& [id, node] : candidate.nodes())
        {
            const auto* old = source.node(id);
            if (old && lux::material::equalMaterialNodes(*node, *old))
                continue;
            auto copy = node->clone();
            if (!copy || !lux::material::equalMaterialNodes(*node, *copy))
                return rejected(EMaterialEditError::CALLBACK, id);
            after.insert.push_back(std::move(copy));
            before.erase.push_back(id);
        }
        const auto affected = [&](const auto& graph, const auto& link, const auto& erased) {
            return std::ranges::find(erased, graph.topology().findPin(link.from)->owner) != erased.end() ||
                   std::ranges::find(erased, graph.topology().findPin(link.to)->owner) != erased.end();
        };
        for (const auto& link : source.topology().links())
        {
            const bool erased = affected(source, link, after.erase);
            const bool removed = !candidate.topology().findLink(link.from, link.to);
            if (erased || removed)
                before.connect.push_back(link);
            if (removed && !erased)
                after.disconnect.push_back(link);
        }
        for (const auto& link : candidate.topology().links())
        {
            const bool erased = affected(candidate, link, before.erase);
            const bool added = !source.topology().findLink(link.from, link.to);
            if (erased || added)
                after.connect.push_back(link);
            if (added && !erased)
                before.disconnect.push_back(link);
        }
        const auto positions = [](const auto& from, const auto& to, auto& delta, const auto& erased) {
            for (const auto& entry : to.layout().all())
            {
                const auto* old = from.layout().find(entry.node);
                const bool replaced = std::ranges::find(erased, entry.node) != erased.end();
                if (!old || *old != entry.layout || replaced)
                    delta.place.push_back(entry);
            }
            for (const auto& entry : from.layout().all())
                if (!to.layout().find(entry.node))
                    delta.unplace.push_back(entry.node);
        };
        positions(source, candidate, after, after.erase);
        positions(candidate, source, before, before.erase);
        return {};
    }

    class MaterialGraphEdit final : public editing::EditOperation
    {
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(
                const MaterialGraphEdit& edit,
                std::optional<lux::material::MaterialGraphEdit> graph,
                std::vector<VMaterialValue> values,
                bool changed
            )
                : edit_(edit), graph_(std::move(graph)), values_(std::move(values)), changed_(changed)
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

        private:
            void apply() noexcept override
            {
                if (graph_)
                    graph_->commit();
                for (auto& value : values_)
                    swapValue(edit_.source_, value);
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                if (edit_.observer_.changed)
                    edit_.observer_.changed(edit_.observer_.owner, info);
            }
            const MaterialGraphEdit& edit_;
            std::optional<lux::material::MaterialGraphEdit> graph_;
            std::vector<VMaterialValue> values_;
            bool changed_;
        };

    public:
        MaterialGraphEdit(
            lux::material::MaterialSource& source,
            editing::StateId base,
            std::string label,
            std::vector<lux::object::CodeLease> code,
            MaterialGraphDelta before,
            MaterialGraphDelta after,
            MaterialValueEdit values,
            MaterialEditObserver observer
        )
            : source_(source), base_(base), label_(std::move(label)), code_(std::move(code)),
              before_(std::move(before)), after_(std::move(after)), values_(std::move(values)), observer_(observer)
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
            return sizeof(*this) + label_.capacity() + 1 + before_.bytes() + after_.bytes() + values_.bytes() +
                   code_.capacity() * sizeof(lux::object::CodeLease);
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        try
        {
            const bool forward = context.direction == editing::EDirection::FORWARD;
            const auto& delta = forward ? after_ : before_;
            const auto& expected = forward ? values_.before : values_.after;
            for (const auto& value : expected)
            {
                auto current = readValue(source_, value);
                if (!current || !equalValue(value, *current))
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            std::size_t staging = sizeof(Plan) + values_.bytes();
            if (!delta.empty())
                staging += 4 * (sourceBytes(source_) + delta.bytes() + 1024);
            if (auto reserved = budget.reserve(staging); !reserved)
                return lux::cxx::unexpected(reserved.error());
            std::optional<lux::material::MaterialGraphEdit> graph;
            if (!delta.empty())
            {
                std::vector<const lux::material::Node*> nodes;
                for (const auto& node : delta.insert)
                    nodes.push_back(node.get());
                auto prepared = lux::material::MaterialGraphEdit::prepare(
                    source_.graph,
                    {nodes, delta.erase, delta.connect, delta.disconnect, delta.place, delta.unplace}
                );
                if (!prepared)
                    return lux::cxx::unexpected(editing::makeEditFailure(
                        editing::EEditError::PRECONDITION_FAILED,
                        static_cast<std::uint64_t>(prepared.error().code)
                    ));
                graph.emplace(std::move(*prepared));
            }
            return editing::PreparedEditPtr(new Plan(
                *this,
                std::move(graph),
                forward ? values_.after : values_.before,
                !delta.empty() || !values_.empty()
            ));
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(EMaterialEditError::CALLBACK)
            ));
        }

    private:
        lux::material::MaterialSource& source_;
        editing::StateId base_;
        std::string label_;
        std::vector<lux::object::CodeLease> code_;
        MaterialGraphDelta before_, after_;
        MaterialValueEdit values_;
        MaterialEditObserver observer_;
    };
}

namespace lux::editor::material
{
    MaterialEditResult<PreparedMaterialEdit> prepareMaterialEdit(
        lux::material::MaterialSource& source,
        editing::StateId base,
        std::vector<VMaterialEdit> edits,
        std::string label,
        lux::object::CodeLease code,
        MaterialEditObserver observer,
        std::size_t staging_limit
    )
    try
    {
        using namespace detail;
        if (!base.valid() || !code.valid())
            return rejected(EMaterialEditError::INVALID_SOURCE);
        std::vector<lux::object::CodeLease> leases{std::move(code)};
        bool structural{};
        for (const auto& edit : edits)
        {
            structural |= edit.index() >= 6;
            if (const auto* node = std::get_if<MaterialInsertNode>(&edit))
                leases.push_back(node->code);
            if (const auto* node = std::get_if<MaterialReplaceNode>(&edit))
                leases.push_back(node->code);
        }
        MaterialValueEdit values;
        MaterialGraphDelta before, after;
        std::vector<lux::material::NodeId> inserted;
        if (structural)
        {
            if (sourceBytes(source) > staging_limit / 4)
                return rejected(EMaterialEditError::BUDGET);
            lux::material::MaterialSource candidate{source.id, source.name, source.graph.clone()};
            if (!lux::material::validateMaterialSource(candidate))
                return rejected(EMaterialEditError::CALLBACK);
            for (auto& edit : edits)
            {
                if (auto applied = applyGraphEdit(candidate, edit, inserted); !applied)
                    return lux::cxx::unexpected(applied.error());
                if (sourceBytes(candidate) > staging_limit / 4)
                    return rejected(EMaterialEditError::BUDGET);
            }
            if (!lux::material::validateMaterialSource(candidate))
                return rejected(EMaterialEditError::INVALID_SOURCE);
            if (auto refs = validateReferences(
                    source,
                    candidate.graph,
                    candidate.graph.texture_slots,
                    candidate.graph.param_slots
                );
                !refs)
                return lux::cxx::unexpected(refs.error());
            auto difference = graphDifference(source.graph, candidate.graph, before, after);
            if (!difference)
                return lux::cxx::unexpected(difference.error());
            std::erase_if(inserted, [&](auto id) { return !candidate.graph.node(id); });
            const std::array<VMaterialValue, 5> metadata{
                MaterialRename{candidate.name},
                MaterialSetShading{candidate.graph.shading_model},
                MaterialSetRenderState{candidate.graph.render_state},
                MaterialSetTextureSlots{candidate.graph.texture_slots},
                MaterialSetParameterSlots{candidate.graph.param_slots}
            };
            for (const auto& value : metadata)
                if (auto set = values.set(source, value); !set)
                    return lux::cxx::unexpected(set.error());
        }
        else
        {
            for (const auto& edit : edits)
            {
                auto set = std::visit(
                    [&](const auto& item) -> MaterialEditResult<void> {
                        if constexpr (std::constructible_from<VMaterialValue, decltype(item)>)
                            return values.set(source, item);
                        else
                            return rejected(EMaterialEditError::INVALID_VALUE);
                    },
                    edit
                );
                if (!set)
                    return lux::cxx::unexpected(set.error());
                if (values.bytes() > staging_limit)
                    return rejected(EMaterialEditError::BUDGET);
            }
            // Validate slot references against the final declarations, without cloning unrelated nodes.
            const auto* textures = &source.graph.texture_slots;
            const auto* parameters = &source.graph.param_slots;
            for (const auto& value : values.after)
            {
                if (const auto* item = std::get_if<MaterialSetTextureSlots>(&value))
                    textures = &item->value;
                if (const auto* item = std::get_if<MaterialSetParameterSlots>(&value))
                    parameters = &item->value;
            }
            if (textures != &source.graph.texture_slots || parameters != &source.graph.param_slots)
            {
                auto refs = validateReferences(source, source.graph, *textures, *parameters);
                if (!refs)
                    return lux::cxx::unexpected(refs.error());
            }
        }
        values.normalize();
        return PreparedMaterialEdit{
            std::make_unique<MaterialGraphEdit>(
                source,
                base,
                std::move(label),
                std::move(leases),
                std::move(before),
                std::move(after),
                std::move(values),
                observer
            ),
            std::move(inserted)
        };
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return detail::rejected(EMaterialEditError::CALLBACK);
    }
}
