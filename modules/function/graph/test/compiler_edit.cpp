#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

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
        material::ConstantNode constant;
        constant.setType(material::EValueType::VEC3);
        constant.value[0] = 0.2F;
        constant.value[1] = 0.5F;
        constant.value[2] = 0.7F;
        material::OutputSurfaceNode surface;
        material::MaterialGraph direct;
        const auto source = direct.addNode(constant.clone());
        const auto destination = direct.addNode(surface.clone());
        require(direct.connect(source, 0, destination, 0));
        auto baseline = material::compileMaterial(direct);
        require(baseline.has_value());

        material::MaterialGraph edited;
        const std::array<material::MaterialNodeEntry, 2> inputs{{{{}, &constant}, {{}, &surface}}};
        material::MaterialGraphChange change;
        change.insert = inputs;
        auto edit = material::MaterialGraphEdit::prepare(edited, change);
        require(edit.has_value());
        require(edit->insertedNodes().size() == 2);
        const auto from = edit->insertedNodes()[0].value->outputs().front().id;
        const auto to = edit->insertedNodes()[1].value->inputs().front().id;
        edit->commit();
        const std::array<graph::LinkRecord, 1> links{{{from, to}}};
        change = {};
        change.connect = links;
        auto connection = material::MaterialGraphEdit::prepare(edited, change);
        require(connection.has_value());
        connection->commit();
        auto actual = material::compileMaterial(edited);
        require(actual.has_value());
        require(actual->gbuffer_spirv == baseline->gbuffer_spirv);
        require(actual->forward_spirv == baseline->forward_spirv);
        material::MaterialGraph invalid;
        constexpr material::NodeId invalid_id{1000000031};
        auto invalid_payload = std::make_unique<material::SampleTextureNode>();
        invalid_payload->texture_slot = 29;
        require(invalid.addNodeWithId(invalid_id, std::move(invalid_payload)) == invalid_id);
        require(invalid.addNode(std::make_unique<material::OutputSurfaceNode>()).valid());
        auto failed = material::compileMaterial(invalid);
        require(!failed && failed.error().code == material::EMaterialCompileError::INVALID_GRAPH);
        require(failed.error().node_id == invalid_id);
        std::printf(
            "Material actual compiler: identical SPIR-V (%zu + %zu words)\n",
            actual->gbuffer_spirv.size(),
            actual->forward_spirv.size()
        );
    }

    void flowCompilation()
    {
        flowforge::FlowGraph direct;
        const auto index = direct.addNodes(std::make_unique<flowforge::OnEventNode>("Tick"));
        require(index != 0U);
        require(direct.addExport({{1}, direct.getNode(index).node->id(), 41, {}}));
        const flowforge::FlowForgeCompileOptions options{.module_name = "shared_graph_edit"};
        auto baseline = flowforge::compileFlowForgeObject(direct, options);
        require(baseline.has_value() && !baseline->object.empty());

        flowforge::FlowGraph edited;
        std::unique_ptr<flowforge::Node> candidate = std::make_unique<flowforge::OnEventNode>("Tick");
        const std::array<std::unique_ptr<flowforge::Node>*, 1> inputs{&candidate};
        flowforge::FlowGraphChange change;
        change.insert = inputs;
        change.preserve_insert_ids = false;
        auto edit = flowforge::FlowGraphEdit::prepare(edited, change);
        require(edit.has_value());
        const auto id = edit->insertedIds().front();
        require(candidate != nullptr);
        edit->commit();
        require(candidate == nullptr);
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
