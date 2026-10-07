#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <thread>

using namespace lux;
using namespace lux::editor;
using namespace std::chrono_literals;

namespace
{
    struct Counts final
    {
        unsigned pane{}, element{}, destroyed{};
    };
    class Probe final : public ui::Pane
    {
    public:
        Probe(Counts& counts, EditorContext* context = nullptr)
            : Pane("Probe"), counts_(counts), context_(context), content_(counts)
        {
            assert(addElement(content_));
        }
        ~Probe() override
        {
            if (context_)
            {
                assert(context_->closing() && context_->closed());
            }
            ++counts_.destroyed;
        }

    private:
        class Content final : public ui::Element
        {
        public:
            explicit Content(Counts& counts) : counts_(counts) {}

        private:
            void update() noexcept override
            {
                ++counts_.element;
            }
            void draw() noexcept override {}
            Counts& counts_;
        };
        void update() noexcept override
        {
            ++counts_.pane;
        }
        Counts& counts_;
        EditorContext* context_;
        Content content_;
    };
} // namespace

int main(int argc, char** argv)
{
    assert(argc == 3);
    const std::string mode = argv[1];
    assert(mode == "global" || mode == "maintenance");
    const auto directory = std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(directory);
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("629c02e8-504e-4ebe-b633-ce8fc2269954")};
    const auto a_file = directory / "Custom Project.luxproj";
    const auto b_file = directory / "B.luxproj";
    assert(writeProjectManifestAtomic(a_file, {1, ids("a"), "A"}, EProjectWrite::REPLACE));
    assert(writeProjectManifestAtomic(b_file, {1, ids("b"), "B"}, EProjectWrite::REPLACE));
    Counts global, a, b;
    EditorConfig config{"Project ownership regression", 480, 320};
    config.layout = {{"probe", "project", "Project"}};
    auto host = LuxEngine::create(
        std::move(config),
        [&](EditorContext& context) noexcept -> FrameworkResult<void>
        {
            return context.ui().registerFactory(
                "probe",
                [&](EditorContext& context,
                    const PaneDescription&) noexcept -> FrameworkResult<std::unique_ptr<ui::Pane>>
                { return std::make_unique<Probe>(context.project().name == "A" ? a : b, &context); }
            );
        }
    );
    assert(host);
    auto& engine = **host;
    auto& root = engine.window().uiRoot();
    auto mounted = root.addPane(std::make_unique<Probe>(global));
    assert(mounted);
    const auto global_handle = root.paneHandle(mounted->get());
    const auto until = [&](auto predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (!predicate())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            auto frame = engine.frame();
            assert(frame && *frame == EFrameStatus::RUNNING);
            std::this_thread::sleep_for(1ms);
        }
    };
    const auto settled = [&]
    {
        auto state = engine.projectStatus().state;
        return state != EProjectTransition::PREPARING && state != EProjectTransition::CLOSING_CURRENT;
    };
    assert(engine.openProject(a_file));
    until(settled);
    assert(engine.context() && engine.context()->project().name == "A");
    if (mode == "global")
    {
        assert(root.resolvePane(global_handle) && global.destroyed == 0);
    }
    assert(engine.context()->project().manifest_file == std::filesystem::canonical(a_file));
    assert(engine.context()->project().root == std::filesystem::canonical(directory));
    std::atomic_bool started{}, release{};
    bool delivered{};
    auto* old = engine.context();
    auto scheduler = engine.engine().execution().blocking();
    assert(scheduler);
    assert(old->tasks().submit(
        {"Held project task", "test"},
        [&](process::TaskReporter reporter) noexcept
        {
            return stdexec::then(
                stdexec::schedule(*scheduler),
                [&, reporter]() noexcept -> FrameworkResult<void>
                {
                    started = true;
                    while (!release.load())
                    {
                        std::this_thread::yield();
                    }
                    assert(reporter.stopToken().stop_requested());
                    return {};
                }
            );
        },
        [&](process::TTaskResult<void, error::Error>&& result) noexcept
        {
            assert(result && a.destroyed == 0);
            delivered = true;
        }
    ));
    until([&] { return started.load(); });
    assert(engine.openProject(b_file));
    until([&] { return old->closing(); });
    const auto before = a;
    const auto global_before = global.pane;
    for (int i{}; i < 5; ++i)
    {
        assert(engine.frame());
    }
    if (mode == "maintenance")
    {
        assert(a.pane == before.pane && a.element == before.element);
    }
    if (mode == "global")
    {
        assert(global.pane > global_before && root.resolvePane(global_handle));
    }
    assert(a.destroyed == 0 && !delivered && engine.context() == old);
    release = true;
    until(settled);
    assert(delivered && a.destroyed == 1 && engine.context()->project().name == "B");
    assert(engine.closeProject());
    until(settled);
    assert(!engine.context() && b.destroyed == 1);
    if (mode == "global")
    {
        assert(root.resolvePane(global_handle) && global.destroyed == 0);
    }
    host->reset();
    assert(global.destroyed == 1);
    std::puts("PASS actual project transitions: global ownership / detached maintenance / settlement lifetime");
}
