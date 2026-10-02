#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/material/MaterialCompilationService.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <thread>
#include <type_traits>

namespace em = lux::editor::material;
namespace ef = lux::editor::flowforge;
using namespace lux;
using namespace lux::editor;

static_assert(!std::is_copy_constructible_v<em::MaterialCompileOperation>);
static_assert(!std::is_copy_assignable_v<em::MaterialCompileOperation>);
static_assert(!std::is_move_constructible_v<em::MaterialCompileOperation>);
static_assert(!std::is_move_assignable_v<em::MaterialCompileOperation>);
static_assert(!std::is_copy_constructible_v<ef::FlowCompileOperation>);
static_assert(!std::is_copy_assignable_v<ef::FlowCompileOperation>);
static_assert(!std::is_move_constructible_v<ef::FlowCompileOperation>);
static_assert(!std::is_move_assignable_v<ef::FlowCompileOperation>);
static_assert(std::is_move_constructible_v<std::unique_ptr<em::MaterialCompileOperation>>);
static_assert(std::is_move_assignable_v<std::unique_ptr<em::MaterialCompileOperation>>);
static_assert(std::is_copy_constructible_v<em::CompiledMaterial>);
static_assert(std::is_copy_constructible_v<ef::CompiledFlow>);

namespace
{
    template <class T> auto take(T result)
    {
        assert(result);
        return std::move(*result);
    }
    template <class F> void until(F predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (!predicate())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void workerFinished(process::ExecutionRuntime& execution, process::TaskId task)
    {
        // Observe worker completion without collecting or dispatching the business result.
        until([&] {
            const auto info = execution.taskInfo(task);
            return info && info->finished.has_value();
        });
    }
    std::size_t dispatch(process::ExecutionRuntime& execution)
    {
        take(execution.collectCompletions());
        return take(execution.dispatchTaskEvents());
    }
    asset::AssetId identity()
    {
        return asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    }
    void checkMaterial(process::ExecutionRuntime& execution)
    {
        sessions::SessionStore authors{1};
        auto slot =
            take(authors.reserve<em::MaterialSession>({"lux.editor.material"}, contracts::CodeLease::builtin()));
        lux::material::MaterialSource source{identity(), "Ownership", {}};
        auto constant = std::make_unique<lux::material::ConstantNode>();
        constant->setType(lux::material::EValueType::VEC3);
        const auto node = source.graph.addNode(std::move(constant));
        const auto output = source.graph.addNode(std::make_unique<lux::material::OutputSurfaceNode>());
        assert(source.graph.connect(node, 0, output, 0));
        auto model = take(em::MaterialSession::create(
            slot.id(),
            sessions::BoundSource{identity(), "author.material"},
            std::move(source)
        ));
        auto* author = model.get();
        assert(authors.prepare(slot, model) && authors.publish(slot));
        const auto before = author->describe();
        const auto bytes = take(take(author->read()).encode());
        auto original = take(em::MaterialCompileOperation::start(execution, take(author->capture())));
        auto abandoned = take(em::MaterialCompileOperation::start(execution, take(author->capture())));
        const auto id = original->id();
        const auto task = original->task();
        const auto key = original->key();
        const auto observed = original->observed();
        assert(abandoned->id() != id && abandoned->task() != task);
        workerFinished(execution, task);
        workerFinished(execution, abandoned->task());
        assert(!original->ready() && !abandoned->ready());
        auto transferred = std::move(original);
        assert(!original && transferred->id() == id && transferred->task() == task);
        std::unique_ptr<em::MaterialCompileOperation> owner;
        owner = std::move(transferred);
        assert(!transferred && owner->key() == key && owner->observed() == observed);
        abandoned.reset(); // Another operation's cleanup must not discard this owner's result.
        std::size_t delivered{};
        until([&] {
            delivered += dispatch(execution);
            return owner->ready();
        });
        assert(delivered == 1 && dispatch(execution) == 0);
        auto result = take(owner->result());
        auto shared_result = result;
        auto value_result = *result;
        owner.reset();
        result.reset();
        assert(shared_result->source->name == "Ownership" && !shared_result->bytes.empty());
        assert(value_result.key == key && std::ranges::equal(value_result.bytes.view(), shared_result->bytes.view()));
        assert(value_result.source == shared_result->source);
        // A later independent operation is still admitted and delivered exactly once.
        auto next = take(em::MaterialCompileOperation::start(execution, take(author->capture())));
        assert(next->id() != id);
        delivered = 0;
        until([&] {
            delivered += dispatch(execution);
            return next->ready();
        });
        assert(delivered == 1 && take(next->result())->source->name == "Ownership");
        em::MaterialCompilationService service(execution, 1);
        assert(take(service.snapshotIds()).empty());
        const auto controlled = take(service.start(take(author->capture())));
        assert(take(service.snapshotIds()) == std::vector{controlled});
        std::thread observer([&] { assert(!service.snapshotIds()); });
        observer.join();
        const auto& operation = take(service.operation(controlled)).get();
        assert(!service.start(take(author->capture())) && !service.acknowledge(controlled));
        workerFinished(execution, operation.task());
        assert(!operation.ready() && !service.acknowledge(controlled));
        until([&] {
            dispatch(execution);
            return operation.ready();
        });
        auto retained = take(operation.result());
        assert(service.acknowledge(controlled) && !service.operation(controlled));
        assert(!retained->bytes.empty() && retained->source->name == "Ownership");
        const auto again = take(service.start(take(author->capture())));
        assert(again != controlled);
        until([&] {
            dispatch(execution);
            return take(service.operation(again)).get().ready();
        });
        assert(service.acknowledge(again));
        assert(take(service.snapshotIds()).empty());
        assert(author->describe().current == before.current && author->describe().dirty == before.dirty);
        assert(author->describe().observed == before.observed);
        assert(take(take(author->read()).encode()) == bytes);
        std::cout << "Material: worker-finished handoff, unique_ptr moves, independent cleanup, exactly-once "
                     "completion, owning result after owner destruction; author unchanged.\n";
    }
    void checkFlow(process::ExecutionRuntime& execution, const char* linker)
    {
        sessions::SessionStore authors{1};
        auto slot = take(authors.reserve<ef::FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
        ef::FlowAuthoringSource source{identity(), "Ownership", {}};
        const auto index = source.graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("tick"));
        assert(source.graph.addExport(
            {lux::flowforge::FlowForgeExportNodeId{1}, source.graph.getNode(index).node->id(), 1234}
        ));
        auto model =
            take(ef::FlowSession::create(slot.id(), sessions::BoundSource{identity(), "author.flow"}, std::move(source))
            );
        auto* author = model.get();
        assert(authors.prepare(slot, model) && authors.publish(slot));
        const auto before = author->describe();
        const auto bytes = take(take(author->read()).encode());
        ef::FlowCompilationService service(execution, 2);
        const auto id =
            take(service.start(take(author->capture()), ef::FlowCompileEnvironment{}, {}, {"missing-P07-linker.exe"}));
        const auto second =
            take(service.start(take(author->capture()), ef::FlowCompileEnvironment{}, {}, {"missing-P07-linker.exe"}));
        assert(id != second);
        assert((take(service.snapshotIds()) == std::vector{id, second}));
        std::thread observer([&] { assert(!service.snapshotIds()); });
        observer.join();
        const auto& operation = take(service.operation(id)).get();
        static_assert(std::is_same_v<decltype(operation), const ef::FlowCompileOperation&>);
        const auto task = operation.task();
        workerFinished(execution, task);
        workerFinished(execution, take(service.operation(second)).get().task());
        assert(!operation.ready());
        auto busy = service.acknowledge(id);
        assert(!busy && std::get<ef::EFlowCompilationError>(busy.error()) == ef::EFlowCompilationError::BUSY);
        auto full = service.start(take(author->capture()));
        assert(!full && std::get<ef::EFlowCompilationError>(full.error()) == ef::EFlowCompilationError::CAPACITY);
        std::size_t delivered{};
        until([&] {
            delivered += dispatch(execution);
            return operation.ready() && take(service.operation(second)).get().ready();
        });
        assert(delivered == 2 && dispatch(execution) == 0);
        assert(!operation.result() && operation.retryable());
        const auto fixed_object = operation.object();
        assert(fixed_object && !fixed_object->object.empty());
        assert(service.acknowledge(second));
        assert(operation.id() == id && operation.task() == task && operation.ready());
        assert(service.retryLink(id, {linker, 2}));
        assert(!operation.ready());
        const auto recovered = take(service.start(take(author->capture()), ef::FlowCompileEnvironment{}, {}, {linker}));
        assert(recovered != id && recovered != second);
        workerFinished(execution, operation.attempts().back().task);
        workerFinished(execution, take(service.operation(recovered)).get().task());
        delivered = 0;
        until([&] {
            delivered += dispatch(execution);
            return operation.ready() && take(service.operation(recovered)).get().ready();
        });
        assert(delivered == 2 && dispatch(execution) == 0);
        assert(operation.object() == fixed_object && operation.attempts().size() == 2);
        assert(operation.attempts()[0].failure && !operation.attempts()[1].failure);
        auto result = take(operation.result());
        auto shared_result = result;
        auto value_result = *result;
        assert(service.acknowledge(id) && !service.operation(id));
        result.reset();
        assert(!shared_result->bytes.empty() && shared_result->source->name == "Ownership");
        assert(
            std::ranges::equal(value_result.bytes.view(), shared_result->bytes.view()) &&
            value_result.artifact == shared_result->artifact
        );
        assert(take(service.operation(recovered)).get().ready());
        assert(take(take(service.operation(recovered)).get().result())->source->name == "Ownership");
        assert(service.acknowledge(recovered));
        assert(take(service.snapshotIds()).empty());
        assert(author->describe().current == before.current && author->describe().dirty == before.dirty);
        assert(author->describe().observed == before.observed);
        assert(take(take(author->read()).encode()) == bytes);
        std::cout
            << "Flow: const borrowing, BUSY/capacity before dispatch, exactly-once completion, independent records, "
               "fixed-object retry, capacity recovery, owning result after acknowledge; author unchanged.\n";
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
    if (std::string_view(argv[1]) == "material")
        checkMaterial(execution);
    else
    {
        assert(std::string_view(argv[1]) == "flow");
        checkFlow(execution, argv[2]);
    }
}
