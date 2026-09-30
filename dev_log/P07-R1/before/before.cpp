#include <lux/engine/editor/material/MaterialPreviewStore.hpp>
#include <lux/engine/editor/material/MaterialSessionAccess.hpp>
#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <lux/engine/editor/flowforge/FlowSessionAccess.hpp>
#include <lux/engine/editor/flowforge/PublishFlowArtifact.hpp>
#include <lux/engine/editor/io/ProjectArtifactStore.hpp>
#include <lux/engine/editor/io/SaveExecution.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <cassert>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <source_location>
using namespace lux;
using namespace lux::editor;
namespace em = lux::editor::material;
namespace ef = lux::editor::flowforge;
namespace
{
    template <class T> auto take(T result)
    {
        if (!result)
        {
            std::cerr << "failed result\n";
            std::abort();
        }
        return std::move(*result);
    }
    template <class F> void until(F predicate, std::source_location caller = std::source_location::current())
    {
        std::cerr << "Waiting at " << caller.line() << std::endl;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (!predicate())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    asset::AssetId identity()
    {
        return asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    }
    lux::material::MaterialSource materialSource()
    {
        lux::material::MaterialSource source{identity(), "S10", {}};
        auto constant = std::make_unique<lux::material::ConstantNode>();
        constant->setType(lux::material::EValueType::VEC3);
        const auto node = source.graph.addNode(std::move(constant));
        const auto output = source.graph.addNode(std::make_unique<lux::material::OutputSurfaceNode>());
        assert(source.graph.connect(node, 0, output, 0));
        return source;
    }
    void rename(em::MaterialSession& session, std::string name)
    {
        em::MaterialEditBatch batch{session.describe().current, "rename", {}};
        batch.edits.push_back(em::MaterialRename{std::move(name)});
        assert(session.apply(std::move(batch)));
    }
    void rename(ef::FlowSession& session, std::string name)
    {
        ef::FlowEditBatch batch{session.describe().current, "rename", {}};
        batch.edits.push_back(ef::FlowRename{std::move(name)});
        assert(session.apply(std::move(batch)));
    }
}

int main(int argc, char** argv)
{
    assert(argc == 3);
    const bool is_material = std::string_view(argv[1]) == "material";
    const bool copy = std::string_view(argv[2]) == "copy";
    auto execution = take(process::ExecutionRuntime::create({.cpu_concurrency = 1, .cpu_queue_capacity = 32,
        .timer = {16}, .blocking = process::BlockingSchedulerConfig{1, 16}}));
    sessions::SessionStore authors{2};
    auto mr = take(authors.reserve<em::MaterialSession>({"lux.editor.material"}, contracts::CodeLease::builtin()));
    auto material = take(em::MaterialSession::create(mr.id(), sessions::BoundSource{identity(), "author.material"}, materialSource()));
    auto* mat = material.get();
    assert(authors.prepare(mr, material) && authors.publish(mr));
    auto fr = take(authors.reserve<ef::FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
    ef::FlowAuthoringSource flow_source{identity(), "Flow S10", {}};
    const auto index = flow_source.graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("tick"));
    assert(flow_source.graph.addExport({lux::flowforge::FlowForgeExportNodeId{1}, flow_source.graph.getNode(index).node->id(), 1234}));
    auto flow = take(ef::FlowSession::create(fr.id(), sessions::BoundSource{identity(), "author.flow"}, std::move(flow_source)));
    auto* author = flow.get();
    assert(authors.prepare(fr, flow) && authors.publish(fr));
    const auto mat_before = mat->describe();
    const auto flow_before = author->describe();
    const auto mat_bytes = take(take(mat->read()).encode());
    const auto flow_bytes = take(take(author->read()).encode());
    ef::FlowCompilationService service(execution, 1);
    std::unique_ptr<em::MaterialCompileOperation> owning;
    ef::FlowCompileId id;
    if (is_material) owning = take(em::MaterialCompileOperation::start(execution, take(mat->capture())));
    else id = take(service.start(take(author->capture()), ef::FlowCompileEnvironment{}, {}, {"missing-P07-linker.exe"}));
    const auto task = is_material ? owning->task() : take(service.operation(id)).get().task();
    until([&] { auto info = execution.taskInfo(task); return info && info->finished.has_value(); });
    const bool before_ready = is_material ? owning->ready() : take(service.operation(id)).get().ready();
    assert(!before_ready);
    std::cout << "worker_finished=1 business_ready_before=0 original_owner_retained=1\n";
    if (copy && is_material) { auto accidental_copy = *owning; }
    if (copy && !is_material) { auto accidental_copy = take(service.operation(id)).get(); }
    std::size_t delivered{};
    for (int i = 0; i != 32; ++i) {
        take(execution.collectCompletions()); delivered += take(execution.dispatchTaskEvents());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool ready = is_material ? owning->ready() : take(service.operation(id)).get().ready();
    bool ack_busy{}, capacity_stuck{};
    if (!is_material) {
        auto ack = service.acknowledge(id);
        ack_busy = !ack && std::get<ef::EFlowCompilationError>(ack.error()) == ef::EFlowCompilationError::BUSY;
        auto next = service.start(take(author->capture()));
        capacity_stuck = !next && std::get<ef::EFlowCompilationError>(next.error()) == ef::EFlowCompilationError::CAPACITY;
    }
    assert(mat->describe().current == mat_before.current && mat->describe().dirty == mat_before.dirty);
    assert(author->describe().current == flow_before.current && author->describe().dirty == flow_before.dirty);
    assert(take(take(mat->read()).encode()) == mat_bytes && take(take(author->read()).encode()) == flow_bytes);
    std::cout << "delivered=" << delivered << " ready=" << ready << " acknowledge_busy=" << ack_busy
              << " capacity_stuck=" << capacity_stuck << " author_unchanged=1\n";
    const bool failed = !ready || delivered != 1 || ack_busy || capacity_stuck;
    return failed ? 1 : 0;
}
