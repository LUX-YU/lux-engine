#include <lux/engine/flowforge/FlowAnalysis.hpp>
#include <lux/engine/flowforge/detail/ExecutionTraversal.hpp>
#include <lux/engine/flowforge/detail/FlowPinSchema.hpp>

#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>
#include <lux/engine/flowforge/script/ScriptEventPayload.hpp>

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lux::flowforge
{
    namespace
    {
        [[nodiscard]] NodeId earlierWitness(NodeId left, NodeId right) noexcept
        {
            if (!left.valid())
            {
                return right;
            }
            if (!right.valid())
            {
                return left;
            }
            return left.value <= right.value ? left : right;
        }

        [[nodiscard]] FlowForgeResult<std::vector<lux::rdesc::ScriptApiRequirement>> deriveAbilityRequirements(
            const FlowGraph& graph,
            ScriptAbilityNodeCatalogView catalog
        ) noexcept
        {
            std::vector<lux::rdesc::ScriptApiRequirement> requirements;
            for (const auto& [id, stored] : graph.nodes())
            {
                const auto* node = stored->payload.get<ScriptAbilityPayload>();
                if (node == nullptr)
                {
                    if (stored->definition->identity().canonical_name == "lux.flow.ability_call")
                    {
                        return lux::cxx::unexpected(FlowForgeFailure{
                            .code = EFlowForgeError::GRAPH_INVALID,
                            .message = "Script Ability operation has no Script Ability node contract",
                            .node_id = id.value
                        });
                    }
                    continue;
                }

                for (const auto& requirement : requirements)
                {
                    const bool is_same_contract = requirement.contract.name() == node->contract().name();
                    const bool has_conflicting_schema =
                        is_same_contract && requirement.expected_schema_hash != node->expectedSchemaHash();
                    if (has_conflicting_schema)
                    {
                        return lux::cxx::unexpected(FlowForgeFailure{
                            .code = EFlowForgeError::SCRIPT_ABILITY_REQUIREMENT_CONFLICT,
                            .message = "the graph uses conflicting schemas for one Script Ability contract",
                            .node_id = id.value
                        });
                    }
                }

                const auto* catalog_node = catalog.find(node->contract(), node->method());
                if (catalog_node == nullptr)
                {
                    bool contract_exists{};
                    for (const auto& candidate : catalog.nodes())
                    {
                        if (candidate.contract == node->contract())
                        {
                            contract_exists = true;
                            break;
                        }
                    }
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = contract_exists ? EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_METHOD
                                                : EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_CONTRACT,
                        .message = contract_exists
                                       ? "the Script Ability method is not present in the supplied catalog"
                                       : "the Script Ability contract is not present in the supplied catalog",
                        .node_id = id.value
                    });
                }
                const bool is_schema_mismatch = catalog_node->schema_version != node->expectedSchemaVersion() ||
                                                catalog_node->schema_hash != node->expectedSchemaHash() ||
                                                catalog_node->kind != node->methodKind();
                if (is_schema_mismatch)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::SCRIPT_ABILITY_SCHEMA_MISMATCH,
                        .message = "the Script Ability node schema does not match the supplied catalog",
                        .node_id = id.value
                    });
                }

                const auto exists = std::ranges::any_of(
                    requirements,
                    [&](const auto& requirement) { return requirement.contract.name() == node->contract().name(); }
                );
                if (!exists)
                {
                    requirements.push_back(
                        {lux::script::ScriptApiContractId{node->contract().name()}, node->expectedSchemaHash()}
                    );
                }
            }
            std::ranges::sort(requirements, {}, [](const auto& requirement) { return requirement.contract.name(); });
            return requirements;
        }

        [[nodiscard]] FlowForgeResult<std::vector<lux::script::ScriptEventSourceDescription>> deriveEventRequirements(
            const FlowGraph& graph,
            std::span<const lux::script::ScriptEventSourceDescription> sources
        ) noexcept
        {
            std::vector<lux::script::ScriptEventSourceDescription> requirements;
            for (const auto& [id, stored] : graph.nodes())
            {
                const auto* node = stored->payload.get<ScriptEventPayload>();
                if (!node)
                {
                    continue;
                }
                const auto& expected = node->source();
                if (!expected.valid())
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::SCRIPT_EVENT_SCHEMA_MISMATCH,
                        .message = "the Script Event source description is invalid",
                        .node_id = id.value
                    });
                }
                const auto found = std::ranges::find_if(
                    sources,
                    [&](const auto& candidate) noexcept
                    {
                        return candidate.system_id == expected.system_id && candidate.event_id == expected.event_id &&
                               candidate.route == expected.route;
                    }
                );
                if (found == sources.end())
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::UNKNOWN_SCRIPT_EVENT_SOURCE,
                        .message = "the Script Event source is not present in the supplied catalog",
                        .node_id = id.value
                    });
                }
                const bool is_schema_mismatch = found->payload != expected.payload ||
                                                found->payload_schema_hash != expected.payload_schema_hash ||
                                                found->payload_schema_version != expected.payload_schema_version ||
                                                found->delivery_hook_id != expected.delivery_hook_id ||
                                                found->delivery_schema_hash != expected.delivery_schema_hash ||
                                                found->delivery_schema_version != expected.delivery_schema_version;
                if (is_schema_mismatch)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::SCRIPT_EVENT_SCHEMA_MISMATCH,
                        .message = "the Script Event source schema does not match the supplied catalog",
                        .node_id = id.value
                    });
                }
                const auto existing = std::ranges::find_if(
                    requirements,
                    [&](const auto& candidate) noexcept
                    { return candidate.system_id == expected.system_id && candidate.event_id == expected.event_id; }
                );
                if (existing == requirements.end())
                {
                    requirements.push_back(expected);
                }
            }
            std::ranges::sort(requirements, lux::script::ScriptEventSourceLess{});
            return requirements;
        }

        [[nodiscard]] std::vector<NodeId> executableConsumers(const FlowGraph& graph, PinId output) noexcept
        {
            std::vector<NodeId> consumers;
            std::queue<PinId> pending;
            std::unordered_set<PinId> visited_pins;
            pending.push(output);
            while (!pending.empty())
            {
                const auto current = pending.front();
                pending.pop();
                if (!visited_pins.insert(current).second)
                {
                    continue;
                }
                for (const auto& link : graph.topology().links())
                {
                    if (link.from != current)
                    {
                        continue;
                    }
                    const auto owner = graph.topology().findPin(link.to)->owner;
                    const bool has_execution = std::ranges::any_of(
                        graph.topology().pins(),
                        [&](const auto& pin) noexcept
                        { return pin.owner == owner && graph.pin(pin.id)->role == EFlowPinRole::EXECUTION; }
                    );
                    if (has_execution)
                    {
                        consumers.push_back(owner);
                        continue;
                    }
                    for (const auto& pin : graph.topology().pins())
                    {
                        const bool is_output = pin.owner == owner && pin.direction == graph::EPinDirection::OUTPUT;
                        if (is_output)
                        {
                            pending.push(pin.id);
                        }
                    }
                }
            }
            return consumers;
        }

        [[nodiscard]] FlowForgeResult<void> validateAbilityLifetimes(
            const FlowGraph& graph,
            const rdesc::ScriptLifecycleRoles& lifecycle,
            const FlowAnalysis& analysis
        ) noexcept
        {
            for (const auto& [id, stored] : graph.nodes())
            {
                const auto* producer = stored->payload.get<ScriptAbilityPayload>();
                if (producer == nullptr)
                {
                    continue;
                }
                for (std::size_t index{}; index < producer->results().size(); ++index)
                {
                    if (producer->results()[index].lifetime != lux::script::EScriptAbilityValueLifetime::BORROWED_STEP)
                    {
                        continue;
                    }
                    const auto output = graph.pinId(
                        id,
                        detail::pinSemantic(EFlowPinRole::DATA, graph::EPinDirection::OUTPUT, index + 1)
                    );
                    for (const auto consumer : executableConsumers(graph, output))
                    {
                        if (const auto suspension = analysis.suspensionBetween(
                                graph.pinId(
                                    id,
                                    detail::pinSemantic(EFlowPinRole::EXECUTION, graph::EPinDirection::OUTPUT, 0)
                                ),
                                consumer
                            );
                            suspension.valid())
                        {
                            return lux::cxx::unexpected(FlowForgeFailure{
                                .code = EFlowForgeError::BORROWED_VALUE_CROSSES_SUSPENSION,
                                .message = "BORROWED_STEP value crosses a Script Ability suspension",
                                .node_id = suspension.value,
                                .pin_id = output.value
                            });
                        }
                    }
                }
            }

            for (const auto& exported : graph.exports())
            {
                const bool is_lifecycle =
                    exported.symbol == lifecycle.begin_play || exported.symbol == lifecycle.end_play;
                if (!is_lifecycle)
                {
                    continue;
                }
                const auto* entry = graph.node(exported.entry_node_id);
                if (entry == nullptr)
                {
                    continue;
                }
                NodeId suspension;
                for (const auto& pin : graph.topology().pins())
                {
                    const bool is_output =
                        pin.owner == exported.entry_node_id && pin.direction == graph::EPinDirection::OUTPUT;
                    if (is_output && graph.pin(pin.id)->role == EFlowPinRole::EXECUTION)
                    {
                        suspension = analysis.firstSuspensionFrom(pin.id);
                    }
                    if (suspension.valid())
                    {
                        break;
                    }
                }
                if (suspension.valid())
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED,
                        .message = "BeginPlay and EndPlay FlowForge exports must remain synchronous",
                        .node_id = suspension.value
                    });
                }
            }
            return {};
        }
    } // namespace

    struct FlowAnalysis::Impl final
    {
        struct ExecutionNode final
        {
            std::vector<NodeId> successors;
            NodeId callee;
            bool suspends{};
            bool foreign_callee{};
            bool incompatible_callee{};
        };

        struct FunctionSummary final
        {
            PinId entry;
            NodeId direct_witness;
            NodeId transitive_witness;
            std::vector<NodeId> callees;
        };

        std::vector<rdesc::ScriptApiRequirement> abilities;
        std::vector<script::ScriptEventSourceDescription> events;
        std::unordered_map<NodeId, ExecutionNode> execution;
        std::unordered_map<PinId, NodeId> starts;
        std::unordered_map<NodeId, FunctionSummary> functions;

        template <class Callback> void visitDirectExecution(PinId start, Callback&& callback) const noexcept
        {
            const auto found = starts.find(start);
            if (found == starts.end())
            {
                return;
            }
            detail::visitExecution(
                std::span{&found->second, 1U},
                [&](NodeId id, auto&& emit) noexcept
                {
                    for (const auto next : execution.at(id).successors)
                    {
                        emit(next);
                    }
                },
                [&](NodeId id) noexcept
                {
                    callback(id, execution.at(id));
                    return true;
                }
            );
        }

        [[nodiscard]] FlowForgeResult<void> build(const FlowGraph& graph) noexcept
        {
            for (const auto& [id, stored] : graph.nodes())
            {
                if (stored->payload.get<FunctionPayload>())
                {
                    functions.emplace(
                        id,
                        FunctionSummary{graph.pinId(
                            id,
                            detail::pinSemantic(EFlowPinRole::EXECUTION, graph::EPinDirection::OUTPUT, 0)
                        )}
                    );
                }
            }

            for (const auto& [id, stored] : graph.nodes())
            {
                auto& projected = execution[id];
                if (const auto* ability = stored->payload.get<ScriptAbilityPayload>())
                {
                    projected.suspends = ability->methodKind() == script::EScriptApiMethodKind::ASYNC_OPERATION;
                }
                else
                {
                    projected.suspends = stored->payload.get<ScriptEventPayload>() != nullptr;
                }

                if (const auto* call = stored->payload.get<FunctionCallPayload>())
                {
                    projected.foreign_callee = !functions.contains(call->callee);
                    if (!projected.foreign_callee)
                    {
                        const auto* definition = graph.node(call->callee)->payload.get<FunctionPayload>();
                        projected.incompatible_callee = !call->matchesSignature(*definition);
                        projected.callee = call->callee;
                    }
                }
            }

            for (const auto& link : graph.topology().links())
            {
                if (graph.pin(link.from)->role != EFlowPinRole::EXECUTION)
                {
                    continue;
                }
                const auto from = graph.topology().findPin(link.from)->owner;
                const auto successor = graph.topology().findPin(link.to)->owner;
                starts.emplace(link.from, successor);
                execution.at(from).successors.push_back(successor);
            }

            for (auto& [id, summary] : functions)
            {
                bool has_foreign_callee{};
                bool has_incompatible_callee{};
                visitDirectExecution(
                    summary.entry,
                    [&](NodeId node_id, const ExecutionNode& node) noexcept
                    {
                        if (node.suspends)
                        {
                            summary.direct_witness = earlierWitness(summary.direct_witness, node_id);
                        }
                        has_foreign_callee = has_foreign_callee || node.foreign_callee;
                        has_incompatible_callee = has_incompatible_callee || node.incompatible_callee;
                        const bool is_new_callee =
                            node.callee.valid() &&
                            std::ranges::find(summary.callees, node.callee) == summary.callees.end();
                        if (is_new_callee)
                        {
                            summary.callees.push_back(node.callee);
                        }
                    }
                );
                if (has_foreign_callee)
                {
                    return cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::GRAPH_INVALID,
                        .message = "graph function call references a definition outside the graph",
                        .node_id = id.value
                    });
                }
                if (has_incompatible_callee)
                {
                    return cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::GRAPH_INVALID,
                        .message = "graph function call signature differs from its definition",
                        .node_id = id.value
                    });
                }
                std::ranges::sort(summary.callees);
                summary.transitive_witness = summary.direct_witness;
            }

            bool changed{};
            do
            {
                changed = false;
                for (auto& [id, summary] : functions)
                {
                    static_cast<void>(id);
                    auto witness = summary.direct_witness;
                    for (const auto callee : summary.callees)
                    {
                        witness = earlierWitness(witness, functions.at(callee).transitive_witness);
                    }
                    if (witness != summary.transitive_witness)
                    {
                        summary.transitive_witness = witness;
                        changed = true;
                    }
                }
            } while (changed);
            return {};
        }

        [[nodiscard]] NodeId callWitness(NodeId id) const noexcept
        {
            const auto& node = execution.at(id);
            if (node.suspends)
            {
                return id;
            }
            const auto found = functions.find(node.callee);
            return found == functions.end() ? NodeId{} : found->second.transitive_witness;
        }
    };

    FlowAnalysis::FlowAnalysis(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    FlowAnalysis::~FlowAnalysis() = default;

    FlowAnalysis::FlowAnalysis(FlowAnalysis&&) noexcept = default;

    FlowAnalysis& FlowAnalysis::operator=(FlowAnalysis&&) noexcept = default;

    FlowForgeResult<FlowAnalysis> FlowAnalysis::create(
        const FlowGraph& graph,
        const FlowAnalysisOptions& options
    ) noexcept
    {
        auto abilities = deriveAbilityRequirements(graph, options.script_abilities);
        if (!abilities)
        {
            return cxx::unexpected(std::move(abilities.error()));
        }
        auto events = deriveEventRequirements(graph, options.script_events);
        if (!events)
        {
            return cxx::unexpected(std::move(events.error()));
        }

        auto impl = std::make_unique<Impl>();
        impl->abilities = std::move(*abilities);
        impl->events = std::move(*events);
        if (auto built = impl->build(graph); !built)
        {
            return cxx::unexpected(std::move(built.error()));
        }
        FlowAnalysis analysis{std::move(impl)};
        if (auto lifetime = validateAbilityLifetimes(graph, options.lifecycle, analysis); !lifetime)
        {
            return cxx::unexpected(std::move(lifetime.error()));
        }
        return analysis;
    }

    std::span<const rdesc::ScriptApiRequirement> FlowAnalysis::abilityRequirements() const noexcept
    {
        return impl_->abilities;
    }

    std::span<const script::ScriptEventSourceDescription> FlowAnalysis::eventRequirements() const noexcept
    {
        return impl_->events;
    }

    NodeId FlowAnalysis::firstSuspensionFrom(PinId start) const noexcept
    {
        NodeId result;
        impl_->visitDirectExecution(
            start,
            [&](NodeId id, const Impl::ExecutionNode&) noexcept
            { result = earlierWitness(result, impl_->callWitness(id)); }
        );
        return result;
    }

    NodeId FlowAnalysis::suspensionBetween(PinId start, NodeId target) const noexcept
    {
        struct Visit final
        {
            NodeId node;
            NodeId suspension;
        };

        std::queue<Visit> pending;
        std::unordered_set<NodeId> visited_without_suspension;
        std::unordered_map<NodeId, NodeId> visited_with_suspension;
        NodeId result;
        if (const auto found = impl_->starts.find(start); found != impl_->starts.end())
        {
            pending.push({found->second, {}});
        }

        while (!pending.empty())
        {
            auto visit = pending.front();
            pending.pop();
            visit.suspension = earlierWitness(visit.suspension, impl_->callWitness(visit.node));
            if (!visit.suspension.valid())
            {
                if (!visited_without_suspension.insert(visit.node).second)
                {
                    continue;
                }
            }
            else
            {
                const auto [found, inserted] = visited_with_suspension.emplace(visit.node, visit.suspension);
                const bool has_earlier_witness = !inserted && found->second.value <= visit.suspension.value;
                if (has_earlier_witness)
                {
                    continue;
                }
                if (!inserted)
                {
                    found->second = visit.suspension;
                }
            }
            if (visit.node == target)
            {
                result = earlierWitness(result, visit.suspension);
                continue;
            }
            for (const auto next : impl_->execution.at(visit.node).successors)
            {
                pending.push({next, visit.suspension});
            }
        }
        return result;
    }
} // namespace lux::flowforge
