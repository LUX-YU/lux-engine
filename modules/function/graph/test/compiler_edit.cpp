#include "../../flowforge/test/FlowTest.hpp"
#include "../../material/test/MaterialTest.hpp"
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/material/Compiler.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;

    void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "contract failed at line %u\n", where.line());
            std::abort();
        }
    }

    void materialCompilation()
    {
        auto constant =
            material_test::make(material::MaterialConstant{{0.2F, 0.5F, 0.7F, 0}, material::EValueType::VEC3});
        auto surface = material_test::make(material::MaterialOutputSurface{});
        material::MaterialGraph direct;
        auto constant_copy = constant.clone();
        auto surface_copy = surface.clone();
        require(constant_copy.has_value() && surface_copy.has_value());
        const auto source = material_test::add(direct, std::move(*constant_copy));
        const auto destination = material_test::add(direct, std::move(*surface_copy));
        require(material_test::connect(direct, source, 0, destination, 0));
        constexpr std::array purposes{rdesc::EMaterialPurpose::GBUFFER, rdesc::EMaterialPurpose::SHADOW};
        auto baseline = material::compileMaterial(direct, purposes);
        require(baseline.has_value());

        material::MaterialGraph edited;
        const std::array<material::MaterialNodeEntry, 2> inputs{{{{}, &constant}, {{}, &surface}}};
        material::MaterialGraphChange change;
        change.insert = inputs;
        auto edit = material::MaterialGraphEdit::prepare(edited, change);
        require(edit.has_value());
        require(edit->insertedNodes().size() == 2);
        const auto from = edit->insertedNodes()[0].pins.front().record.id;
        const auto to = edit->insertedNodes()[1].pins.front().record.id;
        edit->commit();
        const std::array<graph::LinkRecord, 1> links{{{from, to}}};
        change = {};
        change.connect = links;
        auto connection = material::MaterialGraphEdit::prepare(edited, change);
        require(connection.has_value());
        connection->commit();
        auto actual = material::compileMaterial(edited, purposes);
        require(actual.has_value());
        require(
            actual->programs.find(lux::rdesc::EMaterialPurpose::GBUFFER)->shader.stages.front().words ==
            baseline->programs.find(lux::rdesc::EMaterialPurpose::GBUFFER)->shader.stages.front().words
        );
        require(
            actual->programs.find(lux::rdesc::EMaterialPurpose::SHADOW)->shader.stages.front().words ==
            baseline->programs.find(lux::rdesc::EMaterialPurpose::SHADOW)->shader.stages.front().words
        );
        material::MaterialGraph invalid;
        constexpr material::NodeId invalid_id{1000000031};
        auto invalid_payload = material_test::make(material::MaterialSampleTexture{29});
        require(invalid.addNodeWithId(invalid_id, std::move(invalid_payload)).value() == invalid_id);
        require(material_test::add(invalid, material::MaterialOutputSurface{}).valid());
        auto failed = material::compileMaterial(invalid, purposes);
        require(!failed && failed.error().code == material::EMaterialCompileError::INVALID_GRAPH);
        require(failed.error().node_id == invalid_id);
        std::printf(
            "Material actual compiler: identical SPIR-V (%zu + %zu words)\n",
            actual->programs.find(lux::rdesc::EMaterialPurpose::GBUFFER)->shader.stages.front().words.size(),
            actual->programs.find(lux::rdesc::EMaterialPurpose::SHADOW)->shader.stages.front().words.size()
        );
    }

    void flowCompilation()
    {
        flowforge::FlowGraph direct;
        auto baseline_node = flow_test::make("lux.flow.event");
        baseline_node.name = "Tick";
        const auto index = flow_test::add(direct, std::move(baseline_node));
        require(index.valid());
        require(direct.addExport({{1}, index, 41, {}}));
        const flowforge::FlowForgeCompileOptions options{.module_name = "shared_graph_edit"};
        auto baseline = flowforge::compileFlowForgeObject(direct, options);
        require(baseline.has_value() && !baseline->object.empty());

        flowforge::FlowGraph edited;
        auto candidate = flow_test::make("lux.flow.event");
        candidate.name = "Tick";
        const auto definition = candidate.definition;
        const std::array<flowforge::FlowNodeEntry, 1> inputs{{{{}, &candidate}}};
        flowforge::FlowGraphChange change;
        change.insert = inputs;
        auto edit = flowforge::FlowGraphEdit::prepare(edited, change);
        require(edit.has_value());
        const auto id = edit->insertedNodes().front().id;
        require(candidate.definition == definition && candidate.name == "Tick");
        require(edited.node(id) == nullptr);
        edit->commit();
        require(candidate.definition == definition && candidate.name == "Tick");
        require(edited.node(id) != &candidate);
        require(edited.node(id)->definition == definition);
        require(edited.addExport({{1}, id, 41, {}}));
        auto actual = flowforge::compileFlowForgeObject(edited, options);
        require(actual.has_value());
        require(actual->target_triple == baseline->target_triple);
        require(actual->object == baseline->object);
        std::printf("Flow actual compiler: identical object (%zu bytes)\n", actual->object.size());
    }
} // namespace

int main()
{
    materialCompilation();
    flowCompilation();
}
