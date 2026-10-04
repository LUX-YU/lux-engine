#include "ObjectQueue.hpp"
#include <lux/engine/editor/material/MaterialPreview.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/editor/flowforge/PublishFlowArtifact.hpp>
#include <lux/engine/editor/storage/FileArtifactStore.hpp>
#include <lux/engine/editor/persistence/SaveExecution.hpp>
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
    auto execution = take(process::ExecutionRuntime::create(
        {.cpu_concurrency = 1,
         .cpu_queue_capacity = 32,
         .timer = {16},
         .blocking = process::BlockingSchedulerConfig{1, 16}}
    ));
    auto late_execution =
        take(process::ExecutionRuntime::create({.cpu_concurrency = 1, .cpu_queue_capacity = 32, .timer = {16}}));
    auto runtime = take(lux::scene::SceneRuntime::create(execution, {0, 1024}));
    lux::test::ObjectQueue authors_messages;
    sessions::SessionStore authors{authors_messages.dispatcherRef(), 4};
    auto mr = take(authors.reserve<em::MaterialSession>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
    auto material = take(
        em::MaterialSession::create(mr.id(), sessions::BoundSource{identity(), "author.material"}, materialSource())
    );
    auto* mat = material.get();
    assert(authors.prepare(mr, material) && authors.publish(mr));
    em::MaterialPreview preview{*runtime, {}};
    std::atomic_bool entered{}, release{};
    auto block = take(late_execution.submit(
        {"test ordered CPU", "test"},
        [cpu = late_execution.cpu(), &entered, &release](process::TaskReporter) noexcept {
            return stdexec::then(stdexec::schedule(cpu), [&entered, &release]() -> lux::cxx::expected<int, int> {
                entered = true;
                while (!release)
                    std::this_thread::yield();
                return 0;
            });
        },
        [](process::TTaskResult<int, int>&&) noexcept {}
    ));
    until([&] { return entered.load(); });
    rename(*mat, "S10");
    auto s10 = take(em::MaterialCompileOperation::start(late_execution, take(mat->capture()), {}, 1));
    const auto s10_adoption = take(preview.setDesired(s10->key()));
    rename(*mat, "S12");
    const auto before = mat->describe();
    const auto before_source = take(take(mat->read()).encode());
    auto s12 = take(em::MaterialCompileOperation::start(execution, take(mat->capture()), {}, 1));
    const auto s12_adoption = take(preview.setDesired(s12->key()));
    until([&] {
        assert(execution.collectCompletions());
        assert(execution.dispatchTaskEvents());
        return s12->ready();
    });
    auto compiled = take(s12->result());
    assert(compiled->source()->name == "S12");
    assert(preview.receive(s12_adoption, s12->result(), {}));
    assert(preview.status().prepared == s12_adoption);
    release = true;
    until([&] {
        assert(late_execution.collectCompletions());
        assert(late_execution.dispatchTaskEvents());
        return s10->ready();
    });
    assert(preview.receive(s10_adoption, s10->result(), {}));
    assert(preview.status().prepared == s12_adoption);
    auto stale_key = s12->key();
    ++stale_key.environment;
    assert(preview.setDesired(stale_key));
    assert(preview.receive(s12_adoption, s12->result(), {}));
    preview.update();
    assert(!preview.status().prepared);
    auto mismatch = s12_adoption;
    mismatch.target = s10_adoption.target;
    ++mismatch.generation;
    assert(preview.receive(mismatch, s12->result(), {}));
    assert(!preview.status().prepared);
    auto changed_settings = s12->key();
    ++changed_settings.settings.version;
    assert(preview.setDesired(changed_settings));
    assert(preview.receive(s12_adoption, s12->result(), {}));
    assert(!preview.status().prepared);
    em::MaterialPreview second{*runtime, {}};
    const auto first_target = take(preview.setDesired(s12->key()));
    const auto second_target = take(second.setDesired(s12->key()));
    assert(first_target.target != second_target.target && first_target.input == second_target.input);
    assert(preview.receive(first_target, compiled, {}) && second.receive(second_target, compiled, {}));
    assert(preview.status().prepared == first_target && second.status().prepared == second_target);
    assert(second.reset({}));
    const auto rebound = take(second.setDesired(s12->key()));
    assert(rebound.generation > second_target.generation);
    assert(second.receive(second_target, compiled, {}) && !second.status().prepared);
    assert(second.receive(rebound, compiled, {}) && second.status().prepared == rebound);
    (void)second.close();
    assert(!second.setDesired(s12->key()) && preview.status().prepared == first_target);
    assert(mat->describe().current == before.current && mat->describe().dirty == before.dirty);
    assert(take(take(mat->read()).encode()) == before_source);
    // Real compiled input, two owned mesh recipes; no target key is hand-adjusted here.
    {
        em::MaterialPreview recipes{*runtime, {}};
        const auto sphere = take(em::makeSphereMaterialPreviewRecipe());
        const auto original = take(recipes.setDesired(compiled->key(), sphere));
        assert(recipes.receive(original, compiled, {}));
        auto bytes = std::make_shared<const std::vector<std::byte>>(16, std::byte{0x7f});
        const em::MaterialPreviewRecipe other{sphere.mesh, cxx::SharedBytes<>::fromOwner(bytes, *bytes)};
        const auto changed = take(recipes.setDesired(compiled->key(), other));
        assert(changed.input == original.input && changed.recipe > original.recipe);
        assert(changed.generation > original.generation);
        recipes.update();
        assert(!recipes.status().prepared && recipes.receive(original, compiled, {}));
        assert(!recipes.status().prepared);
        assert(recipes.receive(changed, compiled, {}) && recipes.status().prepared == changed);
        assert(take(recipes.setDesired(compiled->key(), other)) == changed);
        assert(take(recipes.setDesired(compiled->key())) == changed); // Compile edits retain the chosen recipe.
        assert(!recipes.setDesired(compiled->key(), em::MaterialPreviewRecipe{}));
        assert(recipes.status().desired == changed && recipes.status().prepared == changed);
        const auto restored = take(recipes.setDesired(compiled->key(), sphere));
        assert(restored.recipe > changed.recipe && recipes.receive(changed, compiled, {}));
        recipes.update();
        assert(!recipes.status().prepared);
        assert(recipes.receive(restored, compiled, {}) && recipes.status().prepared == restored);
    }
    // A real compiler failure must not cause the older successful completion to be relabelled current.
    em::MaterialEditBatch broken{mat->describe().current, "remove output", {}};
    broken.edits.push_back(em::MaterialEraseNode{lux::material::NodeId{2}});
    assert(mat->apply(std::move(broken)));
    auto bad = take(em::MaterialCompileOperation::start(execution, take(mat->capture()), {}, 1));
    const auto bad_adoption = take(preview.setDesired(bad->key()));
    until([&] {
        assert(execution.collectCompletions());
        assert(execution.dispatchTaskEvents());
        return bad->ready();
    });
    assert(!bad->result() && preview.receive(bad_adoption, bad->result(), {}));
    assert(preview.status().compilation_failure);
    assert(preview.receive(s10_adoption, s10->result(), {}));
    assert(!preview.status().diagnostic.empty());
    auto fr = take(authors.reserve<ef::FlowSession>({"lux.editor.flowforge"}, lux::object::CodeLease::builtin()));
    ef::FlowAuthoringSource flow_source{identity(), "Flow S10", {}};
    const auto index = flow_source.graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("tick"));
    assert(flow_source.graph.addExport(
        {lux::flowforge::FlowForgeExportNodeId{1}, flow_source.graph.getNode(index).node->id(), 1234}
    ));
    auto flow =
        take(ef::FlowSession::create(fr.id(), sessions::BoundSource{identity(), "author.flow"}, std::move(flow_source))
        );
    auto* author = flow.get();
    assert(authors.prepare(fr, flow) && authors.publish(fr));
    ef::FlowCompilationService flows(execution, 1);
    rename(*author, "Flow S10");
    const auto frozen = author->describe().current;
    const auto id =
        take(flows.start(take(author->capture()), ef::FlowEnvironment{}, {}, {"missing-P07-linker.exe"}));
    until([&] {
        assert(execution.collectCompletions());
        assert(execution.dispatchTaskEvents());
        return take(flows.operation(id)).get().ready();
    });
    const auto& operation = take(flows.operation(id)).get();
    assert(!operation.result() && operation.retryable());
    const auto object = operation.object();
    assert(object && !object->object.empty());
    assert(!flows.start(take(author->capture())));
    rename(*author, "Flow S12");
    const auto flow_before = author->describe();
    const auto flow_bytes = take(take(author->read()).encode());
    assert(flows.retryLink(id, {argv[1], 2}));
    until([&] {
        assert(execution.collectCompletions());
        assert(execution.dispatchTaskEvents());
        return operation.ready();
    });
    const auto artifact = take(operation.result());
    assert(operation.object() == object && operation.key().content == frozen && artifact->source()->name == "Flow S10");
    assert(operation.attempts().size() == 2 && operation.attempts()[0].failure && !operation.attempts()[1].failure);
    assert(author->describe().current == flow_before.current && take(take(author->read()).encode()) == flow_bytes);
    // Real file IO on the same coordinator/execution adapter used by source saves.
    const auto root =
        std::filesystem::path(argv[2]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    storage::FileArtifactStore disk(root);
    persistence::WriteCoordinator writes;
    persistence::SaveService saves(writes);
    persistence::SaveExecution io(execution, saves, writes, disk);
    const auto material_state = mat->describe();
    auto target = take(disk.resolve("derived.material"));
    auto first = take(em::publishCompiledMaterial(writes, target, compiled));
    auto conflict = take(ef::publishFlowArtifact(writes, target, artifact));
    auto flow_ticket = take(ef::publishFlowArtifact(writes, take(disk.resolve("derived.flow")), artifact));
    until([&] {
        assert(io.submitReady());
        assert(execution.collectCompletions());
        assert(execution.dispatchTaskEvents());
        return take(writes.status(flow_ticket)).stage == persistence::EWriteStage::TERMINAL &&
               take(writes.status(conflict)).stage == persistence::EWriteStage::TERMINAL;
    });
    assert(std::holds_alternative<persistence::CommitReceipt>(*take(writes.status(first)).outcome));
    assert(std::holds_alternative<persistence::NotPublished>(*take(writes.status(conflict)).outcome));
    assert(
        std::get<persistence::NotPublished>(*take(writes.status(conflict)).outcome).failure.code ==
        persistence::EPersistenceError::CONFLICT
    );
    assert(std::filesystem::file_size(root / "derived.material") == compiled->bytes().size());
    assert(std::filesystem::file_size(root / "derived.flow") == artifact->bytes().size());
    assert(mat->describe().current == material_state.current && mat->describe().dirty == material_state.dirty);
    assert(author->describe().current == flow_before.current && author->describe().dirty == flow_before.dirty);
    assert(writes.acknowledge(first) && writes.acknowledge(conflict) && writes.acknowledge(flow_ticket));
    // Admission and retained retry state stay bounded, without recompiling a changed author.
    assert(
        !ef::FlowCompilationService(execution, 1).start(take(author->capture()), ef::FlowEnvironment{}, {1, 1})
    );
    ef::FlowCompilationService retry_limit(execution, 1);
    auto limited =
        take(retry_limit.start(take(author->capture()), ef::FlowEnvironment{}, {}, {"missing-P07-linker.exe"}));
    const auto& limited_operation = take(retry_limit.operation(limited)).get();
    until([&] {
        assert(execution.collectCompletions());
        assert(execution.dispatchTaskEvents());
        return limited_operation.ready();
    });
    const auto fixed_object = limited_operation.object();
    auto mismatched = *fixed_object;
    mismatched.target_triple = fixed_object->target_triple.find("windows") == std::string::npos
                                   ? "x86_64-pc-windows-msvc"
                                   : "x86_64-unknown-linux-gnu";
    const auto object_mismatch = lux::flowforge::linkFlowForgeObject(mismatched, argv[1]);
    assert(!object_mismatch && object_mismatch.error().code == lux::flowforge::EFlowForgeError::LINK_FAILED);
    assert(object_mismatch.error().message.find("object bytes") != std::string::npos);
    assert(limited_operation.object() == fixed_object && limited_operation.retryable());
    for (std::uint64_t attempt = 2; attempt <= 8; ++attempt)
    {
        assert(retry_limit.retryLink(limited, {"missing-P07-linker.exe", attempt}));
        until([&] {
            assert(execution.collectCompletions());
            assert(execution.dispatchTaskEvents());
            return limited_operation.ready();
        });
        assert(limited_operation.object() == fixed_object && limited_operation.attempts().size() == attempt);
    }
    assert(!retry_limit.retryLink(limited, {argv[1], 9}));
    assert(!limited_operation.retryable() && limited_operation.attempts().size() == 8);
    assert(retry_limit.acknowledge(limited));
    assert(flows.acknowledge(id));
    assert(!flows.operation(id));
    std::cout << "X07-04 real Material compilers completed out of order; "
                 "desired/content/config/environment/target/failure verified.\n"
              << "X07-05 real Flow compile, failed linker, fixed-object retry, bounded records and attempt history.\n"
              << "X07-06 real IO and same WriteCoordinator conflict: derived publication preserves both author "
                 "checkpoints.\n";
}
