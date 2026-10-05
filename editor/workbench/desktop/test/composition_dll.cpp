#include "CompositionProbe.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/editor/desktop/EditorContext.hpp>
#include <lux/engine/ui/Root.hpp>

using namespace lux;
using namespace lux::editor::desktop;
int main(int argc, char** argv)
{
    assert(argc == 2);
    fixture::Trace trace;
    using Library = engine::platform::DynamicLibrary;
    auto library = std::shared_ptr<Library>(
        new Library(argv[1]),
        [&](Library* value) noexcept
        {
            assert(trace.windows_destroyed == 4 && trace.destroyed == 1 && trace.destructor_returns == 1);
            delete value;
            ++trace.unloaded;
        }
    );
    assert(library->is_loaded());
    auto read = reinterpret_cast<fixture::ReadDefinitions>(library->get_symbol("compositionDefinitions"));
    assert(read);
    auto execution = process::ExecutionRuntime::create({1, 16, 16, {16}});
    auto messages = object::ObjectMessageQueue::create(32);
    assert(execution && messages);
    std::weak_ptr<fixture::Job> weak;
    {
        EditorContext context{messages->dispatcherRef()};
        auto scope = context.services().createScope();
        assert(scope && scope->provide(services::ServiceNameView{"ec4.execution"}, *execution));
        assert(scope->provide(services::ServiceNameView{"ec4.trace"}, trace));
        std::shared_ptr<const services::ServiceEntry> definition;
        std::vector<std::shared_ptr<const UiEntry>> entries;
        read(object::CodeLease::plugin(library), definition, entries);
        assert(context.services().publish({definition}));
        auto catalog = UiCatalog::prepare(std::move(entries));
        assert(catalog && context.ui().publish(*catalog));
        assert(trace.created == 0 && trace.windows == 0);
        auto root = ui::Root::create(messages->dispatcherRef());
        assert(root);
        auto show = [&](const char* suffix)
        {
            std::vector<std::unique_ptr<ui::Pane, object::ObjectDeleter>> candidates;
            for (std::size_t i{}; i < 2; ++i)
            {
                auto handle = catalog->at(i);
                assert(handle);
                UiCreateInfo input{messages->dispatcherRef(), ui::PaneId{std::to_string(i) + suffix}, {}, {}};
                auto candidate = context.ui().create(*handle, *scope, input);
                assert(candidate && !(*candidate)->attachedRoot());
                candidates.push_back(std::move(*candidate));
            }
            assert((*root)->addSubPanes(candidates));
        };
        show("first");
        auto job = context.services().get<fixture::Job>(*scope);
        assert(job && trace.created == 1 && trace.windows == 2);
        weak = *job;
        auto task = (*job)->start(*job);
        assert(task);
        std::vector<ui::PaneHandle> closing;
        for (auto* pane : (*root)->panes())
        {
            auto handle = (*root)->identify(*pane);
            assert(handle);
            closing.push_back(*handle);
        }
        auto prepared = context.ui().prepareClose(**root, closing);
        assert(prepared && trace.windows_destroyed == 0);
        assert((*root)->commit(*prepared) && trace.windows_destroyed == 0);
        assert(messages->collectRetired() == 2 && trace.windows_destroyed == 2);
        job->reset();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!execution->taskInfo(*task)->finished)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        assert(execution->collectCompletions());
        assert(trace.received == 0);
        assert(execution->dispatchTaskEvents());
        assert(trace.received == 1 && trace.notifications == 0 && trace.destroyed == 0);
        show("reopened");
        assert(trace.created == 1 && trace.windows == 4 && trace.shown == 2);
        auto reopened = context.services().get<fixture::Job>(*scope);
        assert(reopened && *(*reopened)->result() == 73);
        (*reopened)->acknowledge();
        library.reset();
        definition.reset();
        catalog = UiCatalog{};
        auto empty = UiCatalog::prepare({});
        assert(empty && context.ui().publish(std::move(*empty)) && context.services().publish({}));
        for (auto* pane : (*root)->panes())
        {
            auto handle = (*root)->identify(*pane);
            assert(handle);
            auto state = context.ui().captureState(**root, *handle);
            assert(state && state->bytes == std::vector{std::byte{73}});
        }
        assert(trace.operations == 2 && trace.unloaded == 0);
        root->reset();
        assert(trace.windows_destroyed == 4);
        assert(scope->release() && !scope->drained());
        std::jthread worker([last = std::move(*reopened)]() mutable { last.reset(); });
        worker.join();
        assert(weak.expired() && trace.destroyed == 0 && trace.unloaded == 0);
        assert(messages->collectRetired() == 1 && scope->drained() && context.services().drained());
        assert(trace.destroyed == 1 && trace.destructor_returns == 1);
    }
    assert(trace.unloaded == 1);
    weak.reset();
    execution->requestStop();
    assert(execution->join());
    std::cout << "Real DLL: one lazy service, two UI factories, close during Process completion, reopen retained "
                 "result, affinity and code-tail retirement PASS\n";
}
