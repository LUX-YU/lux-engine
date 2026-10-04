#include "CompositionProbe.hpp"
#include "ModuleFixture.hpp"
#include "selectedModules.modules.hpp"
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <lux/engine/ui/Root.hpp>

using namespace lux;
using namespace lux::editor;

namespace
{
    template <class T> auto take(T value)
    {
        assert(value);
        return std::move(*value);
    }
    void exercise(extensions::EditorExtension module)
    {
        fixture::Trace trace;
        auto execution = take(process::ExecutionRuntime::create({1, 16, 16, {16}}));
        auto messages = take(object::ObjectMessageQueue::create(32));
        std::weak_ptr<fixture::Job> weak;
        {
            desktop::EditorContext context{messages.dispatcherRef()};
            extensions::ContributionRegistry contributions{messages.dispatcherRef(), context};
            auto scope = take(context.services().createScope());
            assert(scope.provide(services::ServiceNameView{"ec4.execution"}, execution));
            assert(scope.provide(services::ServiceNameView{"ec4.trace"}, trace));
            auto draft = take(module.contributions());
            assert(draft.services.size() == 1 && draft.ui.size() == 2);
            auto prepared = take(extensions::ContributionSnapshot::prepare(std::move(draft)));
            assert(contributions.enqueue(prepared) && contributions.applyPending());
            assert(trace.created == 0 && trace.windows == 0);
            auto root = take(ui::Root::create(messages.dispatcherRef()));
            auto show = [&](const char* suffix)
            {
                auto construct = [&](const extensions::ContributionSnapshot& fixed
                                 ) -> extensions::ContributionResult<void>
                {
                    std::vector<desktop::UiMountRequest> batch;
                    for (std::size_t i{}; i < 2; ++i)
                    {
                        batch.push_back(
                            {take(fixed.ui().at(i)),
                             {messages.dispatcherRef(), ui::PaneId{std::to_string(i) + suffix}, {}, {}}}
                        );
                    }
                    assert(context.ui().mount(*root, scope, std::move(batch)));
                    return {};
                };
                assert(contributions.withSnapshot(construct));
            };
            show("first");
            auto job = take(context.services().get<fixture::Job>(scope));
            assert(trace.created == 1 && trace.windows == 2);
            weak = job;
            const auto task = take(job->start(job));
            const std::vector<ui::Pane*> closing{root->panes().begin(), root->panes().end()};
            for (auto* pane : closing)
            {
                assert(root->removeSubPane(*pane));
            }
            assert(messages.collectRetired() == 2 && trace.windows_destroyed == 2);
            job.reset();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!execution.taskInfo(task)->finished)
            {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
            assert(execution.collectCompletions());
            assert(trace.received == 0);
            assert(execution.dispatchTaskEvents());
            assert(trace.received == 1 && trace.notifications == 0 && trace.destroyed == 0);
            show("reopened");
            assert(trace.created == 1 && trace.windows == 4 && trace.shown == 2);
            auto reopened = take(context.services().get<fixture::Job>(scope));
            assert(*reopened->result() == 73);
            reopened->acknowledge();
            root.reset();
            assert(trace.windows_destroyed == 4);
            assert(scope.release() && !scope.drained());
            std::jthread worker([last = std::move(reopened)]() mutable { last.reset(); });
            worker.join();
            assert(weak.expired() && trace.destroyed == 0);
            assert(messages.collectRetired() == 1 && scope.drained() && context.services().drained());
            assert(trace.destroyed == 1 && trace.destructor_returns == 1);
        }
        weak.reset();
        execution.requestStop();
        assert(execution.join());
    }

    extensions::EditorExtensionExports candidate;
    const extensions::EditorExtensionExports* table() noexcept
    {
        return &candidate;
    }
    void rejection()
    {
        const extensions::EditorModuleDescriptor descriptor{"test.invalid.module", 1, &table};
        const auto original = *module_fixture::module().exports();
        const auto reject = [&](project::EPluginError error)
        {
            const auto value = extensions::EditorExtension::fromStatic(descriptor);
            assert(!value && value.error().code == error);
        };
        candidate = original;
        candidate.interface_version = 9;
        reject(project::EPluginError::INVALID_EXPORT);
        candidate = original;
        --candidate.structure_size;
        reject(project::EPluginError::INVALID_EXPORT);
        candidate = original;
        candidate.editor_sdk_abi = "different";
        reject(project::EPluginError::ABI_MISMATCH);
        candidate = original;
        candidate.counts.services = 257;
        reject(project::EPluginError::INVALID_EXPORT);
        candidate = original;
        candidate.requires_capabilities.sessions = true;
        reject(project::EPluginError::INVALID_EXPORT);
        const std::array duplicate{&module_fixture::module, &module_fixture::module};
        auto repeated = extensions::loadStaticEditorModules(duplicate);
        assert(!repeated && repeated.error().subject == "editor.module.duplicate");
        const std::array<extensions::GetEditorModule*, 1> missing{};
        assert(!extensions::loadStaticEditorModules(missing));
        assert(!extensions::EditorExtension::fromStatic({{}, 1, &table}));
    }
} // namespace
int main(int argc, char** argv)
{
    assert(argc == 2);
    rejection();
    const auto selected = module_fixture::selectedModules();
    assert(selected.size() == 1 && selected.front() == &module_fixture::module);
    auto modules = take(extensions::loadStaticEditorModules(selected));
    assert(modules.size() == 1 && !modules.front().code());
    exercise(std::move(modules.front()));

    project::PluginDescription description;
    description.identity = {"ec4.module.fixture", 1};
    description.root = std::filesystem::path(argv[1]).parent_path();
    description.runtime_library.path = std::filesystem::path(argv[1]).filename();
    description.runtime_library.sdk_abi = project::pluginSdkAbi();
    description.runtime_library.build_id = "ec4-module";
    description.runtime_library.declaration_digest = "ec4-module";
    description.editor_library = description.runtime_library;
    description.editor_library->exports = {project::EPluginExport::EDITOR};
    auto runtime = take(project::PluginLibrary::load(description));
    auto dynamic = take(extensions::EditorExtension::load(description, *runtime));
    assert(dynamic.code());
    runtime.reset();
    exercise(std::move(dynamic));
    std::cout << "Generated static archive and V10 DLL: identical lazy factory/Process/UI/retirement path PASS\n";
}
