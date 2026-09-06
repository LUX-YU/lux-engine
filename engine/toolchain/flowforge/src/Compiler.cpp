#include <lux/engine/flowforge/Compiler.hpp>

#include <lux/engine/flowforge/compiler/AOT.hpp>
#include <lux/engine/flowforge/compiler/IR.hpp>
#include <lux/engine/flowforge/compiler/SuspensionAnalysis.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <lux/engine/flowforge/script/ScriptGraph.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>

#include <atomic>
#include <algorithm>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>
#include <queue>
#include <unordered_set>

namespace lux::flowforge
{
    FlowForgeResult<std::vector<lux::script::ScriptBindingHint>>
    describeFlowForgeBindingHints(const FlowGraph& graph) noexcept
    {
        if (!validFlowForgeExports(graph))
            return lux::cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "Invalid Script exports"});
        try
        {
            std::vector<lux::script::ScriptBindingHint> result;
            for (const auto& exported : graph.exports())
                for (const auto& target : exported.binding_hints) result.push_back({exported.symbol, target});
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{EFlowForgeError::ALLOCATION_FAILURE, "Binding hints"});
        }
    }
    namespace
    {
        class TemporaryCompileDirectory final
        {
        public:
            TemporaryCompileDirectory() = default;

            explicit TemporaryCompileDirectory(std::filesystem::path path) noexcept
                : path_(std::move(path))
            {
            }

            ~TemporaryCompileDirectory()
            {
                std::error_code error;
                std::filesystem::remove_all(path_, error);
            }

            TemporaryCompileDirectory(const TemporaryCompileDirectory&) = delete;
            TemporaryCompileDirectory& operator=(const TemporaryCompileDirectory&) = delete;
            TemporaryCompileDirectory(TemporaryCompileDirectory&&) noexcept = default;
            TemporaryCompileDirectory& operator=(TemporaryCompileDirectory&&) noexcept = default;

            [[nodiscard]] const std::filesystem::path& path() const noexcept
            {
                return path_;
            }

        private:
            std::filesystem::path path_;
        };

        [[nodiscard]] FlowForgeResult<TemporaryCompileDirectory> createTemporaryDirectory() noexcept
        {
            try
            {
                std::error_code error;
                const auto root = std::filesystem::temp_directory_path(error);
                if (error)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::IO_FAILED,
                        .message = "cannot resolve the temporary directory"
                    });
                }

                static std::atomic<std::uint64_t> next_id{1U};
                for (std::uint32_t attempt{}; attempt < 64U; ++attempt)
                {
                    const auto id = next_id.fetch_add(1U, std::memory_order_relaxed);
                    auto path = root / ("lux-flowforge-" + std::to_string(id));
                    error.clear();
                    if (std::filesystem::create_directory(path, error))
                    {
                        return TemporaryCompileDirectory(std::move(path));
                    }
                }
                return lux::cxx::unexpected(FlowForgeFailure{
                    .code = EFlowForgeError::IO_FAILED,
                    .message = "cannot create a unique FlowForge compile directory"
                });
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
            }
            catch (...)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
            }
        }

        [[nodiscard]] FlowForgeResult<std::vector<std::byte>> readModule(
            const std::filesystem::path& path
        ) noexcept
        {
            try
            {
                std::ifstream stream(path, std::ios::binary | std::ios::ate);
                if (!stream)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::IO_FAILED,
                        .message = "cannot open the linked FlowForge module"
                    });
                }
                const auto end = stream.tellg();
                if (end <= 0)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::IO_FAILED,
                        .message = "the linked FlowForge module is empty"
                    });
                }
                std::vector<std::byte> bytes(static_cast<std::size_t>(end));
                stream.seekg(0, std::ios::beg);
                stream.read(reinterpret_cast<char*>(bytes.data()), end);
                if (!stream)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::IO_FAILED,
                        .message = "cannot read the linked FlowForge module"
                    });
                }
                return bytes;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
            }
            catch (...)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
            }
        }

        [[nodiscard]] FlowForgeResult<std::vector<lux::rdesc::ScriptApiRequirement>> deriveAbilityRequirements(
            const FlowGraph& graph,
            ScriptAbilityNodeCatalogView catalog
        ) noexcept
        {
            try
            {
                std::vector<lux::rdesc::ScriptApiRequirement> requirements;
                for (const auto& storage : graph.nodes())
                {
                    const auto* node = dynamic_cast<const ScriptAbilityNode*>(storage.node.get());
                    if (node == nullptr)
                        continue;

                    for (const auto& requirement : requirements)
                    {
                        if (requirement.contract.name() == node->contract().name() &&
                            requirement.expected_schema_hash != node->expectedSchemaHash())
                        {
                            return lux::cxx::unexpected(FlowForgeFailure{
                                .code = EFlowForgeError::SCRIPT_ABILITY_REQUIREMENT_CONFLICT,
                                .message = "the graph uses conflicting schemas for one Script Ability contract",
                                .node_id = node->id().value
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
                            .code = contract_exists
                                ? EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_METHOD
                                : EFlowForgeError::UNKNOWN_SCRIPT_ABILITY_CONTRACT,
                            .message = contract_exists
                                ? "the Script Ability method is not present in the supplied catalog"
                                : "the Script Ability contract is not present in the supplied catalog",
                            .node_id = node->id().value
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
                            .node_id = node->id().value
                        });
                    }

                    const auto exists = std::ranges::any_of(requirements, [&](const auto& requirement) {
                        return requirement.contract.name() == node->contract().name();
                    });
                    if (!exists)
                    {
                        requirements.push_back({
                            lux::script::ScriptApiContractId{node->contract().name()},
                            node->expectedSchemaHash()
                        });
                    }
                }
                std::ranges::sort(requirements, {}, [](const auto& requirement) {
                    return requirement.contract.name();
                });
                return requirements;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
            }
        }

        [[nodiscard]] FlowForgeResult<std::vector<lux::script::ScriptEventSourceDescription>>
        deriveEventRequirements(
            const FlowGraph& graph,
            std::span<const lux::script::ScriptEventSourceDescription> sources
        ) noexcept
        {
            try
            {
                std::vector<lux::script::ScriptEventSourceDescription> requirements;
                for (const auto& storage : graph.nodes())
                {
                    if (storage.node->operation() != ENodeOperation::SCRIPT_EVENT_WAIT)
                        continue;
                    const auto& node = static_cast<const ScriptEventAwaitNode&>(*storage.node);
                    const auto& expected = node.source();
                    if (!expected.valid())
                    {
                        return lux::cxx::unexpected(FlowForgeFailure{
                            .code = EFlowForgeError::SCRIPT_EVENT_SCHEMA_MISMATCH,
                            .message = "the Script Event source description is invalid",
                            .node_id = node.id().value
                        });
                    }
                    const auto found = std::ranges::find_if(sources, [&](const auto& candidate) noexcept {
                        return candidate.system_id == expected.system_id && candidate.event_id == expected.event_id &&
                            candidate.route == expected.route;
                    });
                    if (found == sources.end())
                    {
                        return lux::cxx::unexpected(FlowForgeFailure{
                            .code = EFlowForgeError::UNKNOWN_SCRIPT_EVENT_SOURCE,
                            .message = "the Script Event source is not present in the supplied catalog",
                            .node_id = node.id().value
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
                            .node_id = node.id().value
                        });
                    }
                    const auto existing = std::ranges::find_if(requirements, [&](const auto& candidate) noexcept {
                        return candidate.system_id == expected.system_id && candidate.event_id == expected.event_id;
                    });
                    if (existing == requirements.end())
                        requirements.push_back(expected);
                }
                std::ranges::sort(requirements, lux::script::ScriptEventSourceLess{});
                return requirements;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
            }
        }

        [[nodiscard]] std::vector<const Node*> executableConsumers(const DataOutPin& output)
        {
            std::vector<const Node*> consumers;
            std::queue<const DataOutPin*> pending;
            std::unordered_set<std::uint64_t> visited_pins;
            pending.push(std::addressof(output));
            while (!pending.empty())
            {
                const auto* current = pending.front();
                pending.pop();
                if (!visited_pins.insert(current->id().value).second)
                    continue;
                for (const auto* input : current->linkPins())
                {
                    const auto* node = input->node();
                    if (!isPureDataOp(node->operation()))
                    {
                        consumers.push_back(node);
                        continue;
                    }
                    for (const auto* pin : node->outPins())
                    {
                        if (pin->kind() == EPinKind::DATA_OUT)
                            pending.push(static_cast<const DataOutPin*>(pin));
                    }
                }
            }
            return consumers;
        }

        [[nodiscard]] FlowForgeResult<void> validateAbilityLifetimes(
            const FlowGraph& graph,
            const FlowForgeCompileOptions& options,
            const SuspensionAnalysis& suspension_analysis
        )
        {
            for (const auto& storage : graph.nodes())
            {
                const auto* producer = dynamic_cast<const ScriptAbilityNode*>(storage.node.get());
                if (producer == nullptr)
                    continue;
                for (std::size_t index{}; index < producer->results().size(); ++index)
                {
                    if (producer->results()[index].lifetime !=
                        lux::script::EScriptAbilityValueLifetime::BORROWED_STEP)
                    {
                        continue;
                    }
                    for (const auto* consumer : executableConsumers(*producer->resultPins()[index]))
                    {
                        if (const auto* suspension =
                                suspension_analysis.suspensionBetween(producer->execOutPin(), *consumer))
                        {
                            return lux::cxx::unexpected(FlowForgeFailure{
                                .code = EFlowForgeError::BORROWED_VALUE_CROSSES_SUSPENSION,
                                .message = "BORROWED_STEP value crosses a Script Ability suspension",
                                .node_id = suspension->id().value,
                                .pin_id = producer->resultPins()[index]->id().value
                            });
                        }
                    }
                }
            }

            for (const auto& exported : graph.exports())
            {
                const bool is_lifecycle = exported.symbol == options.lifecycle.begin_play ||
                    exported.symbol == options.lifecycle.end_play;
                if (!is_lifecycle)
                    continue;
                const auto* entry = graph.findNodeById(exported.entry_node_id);
                if (entry == nullptr)
                    continue;
                const Node* suspension{};
                for (const auto* pin : entry->outPins())
                {
                    if (pin->kind() == EPinKind::EXEC_OUT)
                        suspension = suspension_analysis.firstSuspensionFrom(*static_cast<const ExecOutPin*>(pin));
                    if (suspension != nullptr)
                        break;
                }
                if (suspension != nullptr)
                {
                    return lux::cxx::unexpected(FlowForgeFailure{
                        .code = EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED,
                        .message = "BeginPlay and EndPlay FlowForge exports must remain synchronous",
                        .node_id = suspension->id().value
                    });
                }
            }
            return {};
        }
    }

    FlowForgeResult<lux::script::ScriptArtifact> compileFlowForgeScript(
        const FlowGraph& graph,
        FlowForgeCompileOptions options
    ) noexcept
    {
        try
        {
            if (options.module_name.empty())
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::INVALID_MODULE_NAME});
            }
            if (graph.exports().empty() || !validFlowForgeExports(graph))
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::GRAPH_INVALID});
            }

            auto requirements = deriveAbilityRequirements(graph, options.script_abilities);
            if (!requirements)
                return lux::cxx::unexpected(std::move(requirements.error()));
            auto event_requirements = deriveEventRequirements(graph, options.script_events);
            if (!event_requirements)
                return lux::cxx::unexpected(std::move(event_requirements.error()));
            auto suspension_analysis = SuspensionAnalysis::create(graph);
            if (!suspension_analysis)
                return lux::cxx::unexpected(std::move(suspension_analysis.error()));
            auto lifetime = validateAbilityLifetimes(graph, options, *suspension_analysis);
            if (!lifetime)
                return lux::cxx::unexpected(std::move(lifetime.error()));

            auto context = IRContext::create();
            if (!context)
            {
                return lux::cxx::unexpected(std::move(context.error()));
            }
            auto object = compileToObject(*context, graph, options, *suspension_analysis);
            if (!object)
            {
                return lux::cxx::unexpected(std::move(object.error()));
            }
            const bool is_invalid_state_size = object->state_size > std::numeric_limits<std::uint32_t>::max();
            const bool is_invalid_state_defaults = object->state_defaults.size() > object->state_size;
            if (is_invalid_state_size || is_invalid_state_defaults)
            {
                return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::INVALID_DESCRIPTION});
            }

            auto temporary = createTemporaryDirectory();
            if (!temporary)
            {
                return lux::cxx::unexpected(std::move(temporary.error()));
            }
            const auto module_path = temporary->path() / "flowforge-script.dll";
            auto linked = linkSharedLibrary(*object, module_path, options);
            if (!linked)
            {
                return lux::cxx::unexpected(std::move(linked.error()));
            }
            auto payload = readModule(module_path);
            if (!payload)
            {
                return lux::cxx::unexpected(std::move(payload.error()));
            }

            lux::rdesc::Script description;
            description.schema_version = lux::rdesc::Script::kSchemaVersion;
            description.module_name = options.module_name;
            description.exports = std::move(object->exports);
            description.lifecycle = options.lifecycle;
            description.api_requirements = std::move(*requirements);
            description.event_requirements = std::move(*event_requirements);
            description.body = lux::rdesc::NativeModuleScript{
                LUX_SCRIPT_ABI_VERSION,
                object->state_hash,
                static_cast<std::uint32_t>(object->state_size),
                object->state_align,
                std::move(object->state_defaults)
            };

            auto artifact = lux::script::ScriptArtifact::create(std::move(description), std::move(*payload));
            if (!artifact)
            {
                const auto code = artifact.error() == lux::script::EScriptArtifactError::ALLOCATION_FAILURE
                    ? EFlowForgeError::ALLOCATION_FAILURE
                    : EFlowForgeError::INVALID_DESCRIPTION;
                return lux::cxx::unexpected(FlowForgeFailure{.code = code});
            }
            return std::move(*artifact);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(FlowForgeFailure{.code = EFlowForgeError::FOREIGN_EXCEPTION});
        }
    }
}
