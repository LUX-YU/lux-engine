#include <lux/engine/scene/SceneCompatibility.hpp>

#include <algorithm>
#include <map>
#include <utility>

namespace lux::scene
{
    namespace
    {
        using Key = std::pair<system::ESystemDataKind, std::string_view>;

        struct DataSource final
        {
            const system::SystemDataContract* contract;
            std::size_t system;
        };

        bool before(
            const system::detail::SystemDependencyOrdinalEdge& left,
            const system::detail::SystemDependencyOrdinalEdge& right
        ) noexcept
        {
            return std::pair{left.before, left.after} < std::pair{right.before, right.after};
        }
    } // namespace

    SceneCompatibilityResult resolveSceneData(
        std::span<const SceneSystemContract> systems,
        std::span<const system::SystemDataContract> schemas,
        std::span<const system::detail::SystemDependencyOrdinalEdge> explicit_dependencies
    ) noexcept
    {
        const auto failed =
            [&](ESceneCompatibilityError code, std::size_t consumer, std::string_view data, std::size_t producer
            ) -> SceneCompatibilityResult
        {
            return cxx::unexpected(SceneCompatibilityFailure{
                code,
                consumer < systems.size() ? systems[consumer].instance : system::SystemInstanceId{},
                producer < systems.size() ? systems[producer].instance : system::SystemInstanceId{},
                consumer < systems.size() && systems[consumer].is_simulation,
                std::string{data}
            });
        };
        const auto absent = systems.size();
        if (!system::validDataContracts(schemas))
        {
            return failed(ESceneCompatibilityError::INVALID_DECLARATION, absent, {}, absent);
        }
        SceneDataDependencies edges(explicit_dependencies.begin(), explicit_dependencies.end());
        std::sort(edges.begin(), edges.end(), before);
        const auto is_selected = [&](std::size_t producer, std::size_t consumer) noexcept
        {
            return std::binary_search(
                edges.begin(),
                edges.end(),
                system::detail::SystemDependencyOrdinalEdge{producer, consumer},
                before
            );
        };
        std::map<Key, std::vector<DataSource>> sources;
        for (const auto& schema : schemas)
        {
            if (schema.kind != system::ESystemDataKind::SCHEMA)
            {
                return failed(ESceneCompatibilityError::INVALID_DECLARATION, absent, schema.name, absent);
            }
            sources[{schema.kind, schema.name}].push_back({&schema, absent});
        }
        std::vector<system::SystemInstanceId> ordinals;
        ordinals.reserve(systems.size());
        for (std::size_t index{}; index < systems.size(); ++index)
        {
            const auto& selected = systems[index];
            const bool is_invalid_identity = !selected.instance.valid();
            const bool is_invalid_description = selected.description == nullptr ||
                                                !system::validSystemTypeDescription(*selected.description);
            if (is_invalid_identity || is_invalid_description)
            {
                return failed(ESceneCompatibilityError::INVALID_DECLARATION, index, {}, absent);
            }
            ordinals.push_back({index + 1}); // Distinguish equal numeric IDs from the two domains.
            for (const auto& output : selected.description->data_outputs)
            {
                const bool is_prepared = output.kind == system::ESystemDataKind::PREPARED_DATA;
                const bool is_simulation_phase = output.phase == system::ESystemDataPhase::SIMULATION;
                if (is_prepared && is_simulation_phase != selected.is_simulation)
                {
                    return failed(ESceneCompatibilityError::INVALID_DECLARATION, index, output.name, absent);
                }
                sources[{output.kind, output.name}].push_back({&output, index});
            }
        }
        SceneDataDependencies inferred;
        for (std::size_t index{}; index < systems.size(); ++index)
        {
            const auto& selected = systems[index];
            for (const auto& input : selected.description->data_inputs)
            {
                const bool is_schema = input.kind == system::ESystemDataKind::SCHEMA;
                const bool is_simulation_phase = input.phase == system::ESystemDataPhase::SIMULATION;
                if (!is_schema && is_simulation_phase != selected.is_simulation)
                {
                    return failed(ESceneCompatibilityError::INVALID_DECLARATION, index, input.name, absent);
                }
                const auto found = sources.find({input.kind, input.name});
                if (found == sources.end())
                {
                    return failed(
                        is_schema ? ESceneCompatibilityError::MISSING_SCHEMA
                                  : ESceneCompatibilityError::MISSING_PRODUCER,
                        index,
                        input.name,
                        absent
                    );
                }
                const DataSource* source{};
                std::size_t matches{}, explicit_matches{};
                for (const auto& candidate : found->second)
                {
                    if (candidate.contract->version != input.version)
                    {
                        continue;
                    }
                    ++matches;
                    if (is_schema)
                    {
                        source = &candidate;
                        break;
                    }
                    if (candidate.system != absent && is_selected(candidate.system, index))
                    {
                        source = &candidate;
                        ++explicit_matches;
                    }
                    else if (explicit_matches == 0)
                    {
                        source = &candidate;
                    }
                }
                if (matches == 0)
                {
                    return failed(ESceneCompatibilityError::VERSION_MISMATCH, index, input.name, absent);
                }
                if (is_schema)
                {
                    continue;
                }
                const bool is_ambiguous = explicit_matches > 1 || (explicit_matches == 0 && matches > 1);
                if (is_ambiguous)
                {
                    return failed(ESceneCompatibilityError::AMBIGUOUS_PRODUCER, index, input.name, absent);
                }
                if (source->contract->phase > input.phase)
                {
                    return failed(ESceneCompatibilityError::PHASE_INVERSION, index, input.name, source->system);
                }
                if (source->system != index)
                {
                    inferred.push_back({source->system, index});
                }
                else if (source->contract->phase == input.phase)
                {
                    return failed(ESceneCompatibilityError::DEPENDENCY_CYCLE, index, input.name, index);
                }
            }
        }
        edges.insert(edges.end(), inferred.begin(), inferred.end());
        std::sort(edges.begin(), edges.end(), before);
        edges.erase(
            std::unique(
                edges.begin(),
                edges.end(),
                [](const auto& left, const auto& right) noexcept
                { return left.before == right.before && left.after == right.after; }
            ),
            edges.end()
        );
        auto ordered = system::detail::deterministicSystemOrder(ordinals, edges);
        if (!ordered)
        {
            return failed(
                ordered.error() == system::detail::ESystemDependencyOrderError::CYCLE
                    ? ESceneCompatibilityError::DEPENDENCY_CYCLE
                    : ESceneCompatibilityError::INVALID_DECLARATION,
                absent,
                {},
                absent
            );
        }
        return edges;
    }
} // namespace lux::scene
