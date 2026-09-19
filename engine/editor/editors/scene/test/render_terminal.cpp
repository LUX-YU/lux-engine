#include "render_thread_checks.hpp"
#include <lux/engine/editor/gui/shell/EditorWindow.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>

struct StagedOwner final
{
    std::size_t *retired;
    explicit StagedOwner(std::size_t *value) : retired(value) {}
    ~StagedOwner()
    {
        ++*retired;
    }
};

// Real Render backend shutdown through its existing terminal channel, without
// device-loss emulation. A stopping intent alone never proves resource
// retirement.
int main(int argc, char **argv)
{
    using namespace lux;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(argc == 2);
    const bool pending = std::string_view(argv[1]) == "pending";
    meta::ReflectionRegistry::initRegistry();
    window::GlfwRuntime platform;
    assert(platform.valid());
    object::ObjectMessageQueue queue;
    editor::gui::WindowSpec spec;
    spec.visible = false;
    auto window = editor::gui::EditorWindow::create(queue.dispatcherRef(), spec);
    if (!window)
    {
        std::printf("window create failed code=%u\n", unsigned(window.error().code));
    }
    assert(window);
    editor::rendering::RendererConfig config;
    config.validation = true;
    auto renderer =
        editor::rendering::EditorRenderer::create((*window)->nativeWindow(), (*window)->uiSession(), config);
    assert(renderer);
    auto runtime = process::ExecutionRuntime::create({2, 64, 64, {64}});
    assert(runtime);
    auto lease = (*renderer)->acquire();
    assert(lease);
    RenderThreadChecks fixture;
    fixture.begin(**renderer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto poll = [&]
    {
        assert(std::chrono::steady_clock::now() < deadline);
        assert((*renderer)->poll(64));
        std::this_thread::yield();
    };
    while (fixture.binding->state() != scene::ESceneRenderBindingState::READY)
    {
        poll();
        fixture.binding->poll(0);
    }
    auto input = fixture.binding->takeInput();
    assert(input);
    auto view = (*renderer)->openView(input->sceneId(), {{320, 240}, true, 2048});
    assert(view);
    simulation::ecs::Registry registry;
    auto pipeline = input->makePipeline(registry, fixture.description.systemAt(0));
    assert(pipeline);
    const auto entity = registry.create();
    registry.emplace<simulation::ecs::WorldTransform3D>(entity);
    registry.emplace<simulation::ecs::Light3D>(entity);
    assert((*pipeline)->tryPublish() == scene::ERenderPublishResult::FULL_SYNC_PUBLISHED);
    if (pending)
    {
        registry.patch<simulation::ecs::Light3D>(entity, [](auto &light) { light.value.intensity = 2; });
        assert((*pipeline)->tryPublish() == scene::ERenderPublishResult::BACKPRESSURED);
    }
    if (!pending)
    {
        while (fixture.binding->statistics().forwarded == 0)
        {
            poll();
            fixture.binding->poll(1);
        }
    }
    std::size_t staged_retired{};
    assert(lease->programs().beginFrame());
    static_cast<void>(lease->programs().builder().emplaceAttachment<StagedOwner>(render::attachment_types::OwnedObject,
                                                                                 &staged_retired));
    const auto terminal = render::renderError<render::err::comm::ChannelStopping>();
    lease->programs().progressDomain()->publishTerminalError(terminal);
    lease->programs().requestStop();
    fixture.binding->poll(0); // Lifecycle observation must also work with a zero packet budget.
    assert(staged_retired == 0 && (*renderer)->statistics().runtime_leases == 2);
    assert((*pipeline)->tryPublish() == scene::ERenderPublishResult::FAILED);
    pipeline->reset();
    assert((*view)->beginClose());
    while (*(*view)->advanceClose() != editor::rendering::ERenderClose::COMPLETE)
    {
        poll();
    }
    view->reset();
    while ((*renderer)->state() != editor::rendering::ERendererState::FAILED)
    {
        poll();
    }
    poll(); // Completes Main retirement after the backend's release/acquire
            // terminal fact.
    fixture.binding->requestClose();
    for (unsigned i = 0; i < 8; ++i)
    {
        fixture.binding->poll(1);
    }
    const auto counts = fixture.binding->statistics();
    std::printf("JR01 mode=%s backend=FAILED producer=ended view=closed binding=%u "
                "published=%llu forwarded=%llu pending=%u leases=%zu\n",
                argv[1], unsigned(fixture.binding->state()), static_cast<unsigned long long>(counts.published),
                static_cast<unsigned long long>(counts.forwarded), counts.pending,
                (*renderer)->statistics().runtime_leases);
    assert(fixture.binding->state() == scene::ESceneRenderBindingState::CLOSED &&
           "JR01 backend retired but Binding cannot close");
    assert(fixture.binding->hasFailure());
    assert(fixture.binding->failure().render.type == terminal.type);
    assert(fixture.binding->failure().render.args == terminal.args);
    assert(counts.forwarded == (pending ? 0U : 1U) && counts.pending == 0);
    assert(counts.retired_unforwarded == (pending ? 1U : 0U) && staged_retired == 1);
    fixture.binding.reset();
    *lease = {};
    assert((*renderer)->statistics().runtime_leases == 0);
    assert((*renderer)->beginClose());
    while (*(*renderer)->advanceClose() != editor::rendering::ERenderClose::COMPLETE)
    {
        poll();
    }
    assert((*renderer)->statistics().validation_errors == 0);
    assert((*renderer)->joinStopped());
    renderer->reset();
    assert((*window)->closeAfterRendererStopped());
    window->reset();
    runtime->requestStop();
    assert(runtime->join());
    std::puts("PASS JR01 actual backend retired; original terminal error "
              "retained; no new drain; all owners closed");
}
