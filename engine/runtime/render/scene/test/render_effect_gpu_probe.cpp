#include <lux/engine/runtime/extensions/RenderEffects.hpp>
#include <lux/engine/runtime/render/scene/BuiltinRenderEffects.hpp>

#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>

#include "DeviceRenderFixture.hpp"

#include <algorithm>
#include <cstdio>

namespace
{
    int failures = 0;

    void expect(bool value, const char* message)
    {
        std::fprintf(stderr, "[%s] %s\n", value ? " ok " : "FAIL", message);
        if (!value)
            ++failures;
    }

    bool drive(
        lux::runtime::RenderEffectHost& host,
        lux::render::RenderControlSession& control,
        const lux::runtime::RenderEffectOperationTicket& ticket)
    {
        for (;;)
        {
            (void)host.processSafePoint();
            const auto snapshot = ticket.snapshot();
            if (snapshot.terminal ==
                lux::extensions::EOperationTerminalState::SUCCEEDED)
                return true;
            if (snapshot.terminal !=
                lux::extensions::EOperationTerminalState::PENDING)
            {
                std::fprintf(
                    stderr,
                    "[diag] render effect terminal=%u phase=%u error=%u\n",
                    static_cast<unsigned>(snapshot.terminal),
                    static_cast<unsigned>(snapshot.phase),
                    snapshot.error
                        ? static_cast<unsigned>(*snapshot.error)
                        : 0u);
                return false;
            }
            if (!control.waitAndPumpReplies())
                return false;
        }
    }
}

int main()
{
    lux::rendertest::DeviceRenderFixture fixture(
        32u,
        32u,
        "render_effect_gpu_probe");
    if (!fixture.ok())
    {
        std::fprintf(stderr, "[skip] Vulkan device unavailable\n");
        return 0;
    }

    const auto scene = fixture.awaitControl(
        fixture.control().createScene("RenderEffectProbe"));
    fixture.awaitControl(
        fixture.control().setActiveScene(scene.scene_id, true));

    lux::ecs::World world;
    lux::ecs::Schedule schedule(world);
    lux::ecs::SceneServices services;
    lux::ecs::RenderSystemBuilder render_builder;
    auto plan = std::move(render_builder).compile();
    expect(plan.has_value(), "an empty base extraction plan is valid");
    if (!plan)
        return 1;

    lux::render::FeatureCatalog feature_catalog;
    auto render = std::make_unique<lux::ecs::RenderSystem>(
        fixture.session(),
        fixture.control(),
        fixture.uploadClientForTest(),
        fixture.control().adoptScene(scene.scene_id),
        std::move(*plan));
    render->setFeatures(feature_catalog);

    lux::ecs::ScheduleBuilder schedule_builder(schedule, services);
    auto pending_render = schedule_builder.add(
        std::move(render),
        lux::ecs::kPhaseRender);
    expect(
        pending_render.has_value() && schedule_builder.commit().has_value(),
        "the Schedule owns exactly one top-level RenderSystem");
    if (!pending_render)
        return 1;
    auto render_handle = schedule_builder.handle(*pending_render);
    auto* render_system = schedule.get(render_handle);
    expect(render_system != nullptr, "RenderSystem handle resolves");
    if (!render_system)
        return 1;

    lux::runtime::RenderEffectCatalog catalog;
    expect(
        lux::runtime::addGrid3DRenderEffect(catalog).has_value(),
        "Grid3D registers as a render-effect contribution");
    lux::runtime::RenderEffectTypeRegistry types(fixture.control());
    lux::runtime::RenderEffectHost host(
        *render_system,
        services,
        scene.scene_id,
        fixture.control(),
        feature_catalog,
        catalog,
        types);

    auto enabled = host.facade().requestEnable(
        lux::render::renderEffectId(
            "org.lux.render.grid3d.effect"));
    const bool enable_ok = drive(host, fixture.control(), enabled);
    if (!enable_ok)
    {
        const auto* descriptor = catalog.find(
            lux::render::renderEffectId(
                "org.lux.render.grid3d.effect"));
        const auto direct = fixture.awaitControl(
            fixture.control().addFeatureRaw(
                scene.scene_id,
                feature_catalog.typeId("Grid3DPass"),
                descriptor->default_config.bytes));
        std::fprintf(
            stderr,
            "[diag] direct Grid3D add: feature=%d error=%s\n",
            direct.feature.isValid() ? 1 : 0,
            lux::render::formatRenderError(
                lux::render::renderErrorRegistry(),
                direct.error).c_str());
        if (direct.feature.isValid())
            (void)fixture.awaitControl(
                fixture.control().removeFeature(
                    scene.scene_id,
                    direct.feature));
    }
    expect(
        enable_ok &&
            host.active(lux::render::renderEffectId(
                "org.lux.render.grid3d.effect")),
        "enable registers the type, creates the scene instance, and installs extraction");
    expect(
        std::ranges::find(
            render_system->renderFeatures(),
            std::string_view{"Grid3DPass"}) !=
            render_system->renderFeatures().end(),
        "the rebuilt execution plan exposes Grid3D without a second ISystem");

    auto disabled = host.facade().requestDisable(
        lux::render::renderEffectId(
            "org.lux.render.grid3d.effect"));
    expect(
        drive(host, fixture.control(), disabled) &&
            !host.active(lux::render::renderEffectId(
                "org.lux.render.grid3d.effect")),
        "disable removes extraction before the render-thread feature instance");
    expect(
        host.close().terminal(),
        "an inactive RenderEffectHost closes without polling or sleeping");
    return failures == 0 ? 0 : 1;
}
