#include "render_thread_checks.hpp"
#include <lux/engine/meta/Meta.hpp>

struct StagedOwner final
{
    std::size_t *retired;
    explicit StagedOwner(std::size_t *value) : retired(value)
    {
    }
    ~StagedOwner()
    {
        ++*retired;
    }
};

// Real channel shutdown, not physical device loss. The Scene/System is the
// production owner and the result receipt survives its destruction.
int main(int argc, char **argv)
{
    using namespace lux;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(argc == 2);
    const bool pending = std::string_view(argv[1]) == "pending";
    assert(pending || std::string_view(argv[1]) == "forwarded");
    meta::ReflectionRegistry::initRegistry();
    scene::initializeBuiltinRenderSystemMeta();
    render::initializeBuiltinRenderFeatureMeta();
    render::RendererConfig config;
    config.validation = true;
    config.feature_factories = {render::kLightFeatureFactory};
    auto made_runtime = render::RenderRuntime::create(std::move(config));
    assert(made_runtime);
    auto runtime = std::move(*made_runtime);
    RenderThreadChecks fixture;
    fixture.begin(*runtime);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto poll = [&] {
        assert(std::chrono::steady_clock::now() < deadline);
        std::size_t controls = 8, programs = 2;
        assert(runtime->poll(32, controls, programs));
        std::this_thread::yield();
    };
    while (fixture.receipt.status().state != render::ESceneResourceState::READY)
    {
        poll();
    }
    auto opened = fixture.system->openView({.extent = {320, 240}});
    assert(opened);
    auto view = std::move(*opened);
    auto &registry = fixture.instance->registry();
    const auto entity = registry.create();
    registry.emplace<simulation::ecs::WorldTransform3D>(entity);
    registry.emplace<simulation::ecs::Light3D>(entity);
    scene::SceneAdvanceBudget budget{32, 1, pending ? 0U : 1U};
    static_cast<void>(fixture.driver.advance(*fixture.instance, std::chrono::steady_clock::now(), budget));
    while (!pending && fixture.system->transportStatistics().forwarded == 0)
    {
        poll();
        budget = {32, 1, 1};
        static_cast<void>(fixture.driver.advance(*fixture.instance, std::chrono::steady_clock::now(), budget));
    }
    const auto before = fixture.system->transportStatistics();
    assert(before.published == 1 && before.forwarded == (pending ? 0U : 1U));
    assert(before.pending == (pending ? 1U : 0U));

    // One independently admitted attachment proves CPU program ownership is
    // preserved until Main observes backend retirement. No new UI frame is needed.
    std::size_t retired{};
    render::RenderProgram<> program;
    render::RenderProgramSession::Builder builder(program);
    builder.begin({});
    program.kind = render::ERenderProgramKind::StateUpdate;
    builder.emplaceAttachment<StagedOwner>(render::attachment_types::OwnedObject, &retired);
    builder.emplaceAttachment<render::RenderSceneLease>(902, fixture.system->retainScene());
    while (*runtime->submit(program) != render::EFrameSubmit::SUBMITTED)
    {
        poll();
    }
    assert(retired == 0);
    auto control = runtime->control();
    assert(control);
    control->get().requestStop();
    assert(retired == 0); // Stop intent cannot destroy client-side ring storage.
    budget = {32, 1, 0};
    static_cast<void>(fixture.driver.advance(*fixture.instance, std::chrono::steady_clock::now(), budget));
    const auto result = fixture.instance->progress().result;
    assert(!result);
    const auto &stage = std::get<scene::SceneExecutionFailure>(result.error().cause);
    const auto terminal = std::any_cast<render::RenderError>(stage.cause);
    const auto expected = render::renderError<render::err::comm::ChannelStopping>();
    assert(terminal.type == expected.type && terminal.args == expected.args);
    assert(fixture.system->transportStatistics().forwarded == before.forwarded);
    fixture.instance.reset();
    fixture.system = nullptr;
    assert(view->beginClose());
    while (*view->advanceClose() != render::ERenderClose::COMPLETE)
    {
        poll();
    }
    view.reset();
    while (fixture.receipt.status().state != render::ESceneResourceState::RETIRED)
    {
        poll();
    }
    const auto final = fixture.receipt.status();
    assert(final.failure.type == terminal.type && final.failure.args == terminal.args);
    assert(retired == 1 && runtime->statistics().runtime_leases == 0 && runtime->statistics().views == 0);
    assert(runtime->beginClose());
    for (;;)
    {
        std::size_t replies = 32, controls = 8, programs = 2;
        const auto close = runtime->advanceClose(replies, controls, programs);
        assert(close);
        if (*close == render::ERenderClose::COMPLETE)
        {
            break;
        }
        poll();
    }
    assert(runtime->statistics().validation_errors == 0 && runtime->joinStopped());
    std::printf("PASS terminal mode=%s published=%llu forwarded=%llu pending_at_stop=%u "
                "SceneInstance destroyed; View/Program/Scene retired; original error retained\n",
                argv[1], before.published, before.forwarded, before.pending);
}
