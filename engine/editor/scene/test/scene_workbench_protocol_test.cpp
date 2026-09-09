#include <lux/engine/editor/scene/SceneCamera.hpp>
#include <lux/engine/editor/scene/ResourceRequestKey.hpp>
#include <lux/engine/scene/RenderSyncPipeline.hpp>
#include <lux/engine/ui/UISession.hpp>
#include <lux/engine/ui/ViewportElement.hpp>
#include <cassert>

namespace
{
    class Stage final : public lux::scene::RenderSyncStage
    {
    public:
        bool dirty{true};
        bool hasPendingChanges() const noexcept override { return dirty; }
        void requestFullSync() noexcept override { dirty = true; }
        lux::scene::ERenderSyncPrepareResult prepare(lux::render::RenderProgramBuilder<>& builder) noexcept override
        {
            builder.push(lux::render::opcodes::CommandOp, 7, std::uint32_t{42});
            return lux::scene::ERenderSyncPrepareResult::PREPARED_COMMANDS;
        }
        void commitPrepared() noexcept override { dirty = false; }
        void discardPrepared() noexcept override {}
    };

    void pendingStateSurvivesFrameBackpressure()
    {
        using namespace lux;
        auto channel = render::RenderProgramChannel<>::create(1);
        auto sync = std::make_shared<render::RenderChannelSync>();
        render::RenderProgramSession session{channel, sync};
        scene::RenderSyncPipeline::StageList stages;
        stages.push_back(std::make_unique<Stage>());
        auto pipeline = scene::RenderSyncPipeline::create(std::move(stages));
        assert(pipeline);
        assert(session.beginFrame());
        assert(session.trySubmitFrame());
        assert(session.beginFrame());
        assert(!session.trySubmitFrame());
        assert((*pipeline)->tryPublish() == scene::ERenderPublishResult::FULL_SYNC_PUBLISHED);
        assert((*pipeline)->hasPendingUpdate());
        assert((*pipeline)->tryForwardUpdate(session) == scene::ERenderForwardResult::BACKPRESSURED);
        unsigned updates{};
        for (unsigned step = 0; step < 12; ++step)
        {
            if (channel->requests.tryAcquireRead())
            {
                const auto& program = channel->requests.currentRead();
                if (program.kind == render::ERenderProgramKind::StateUpdate)
                {
                    ++updates;
                    assert(program.commands.size() == 1);
                }
            }
            static_cast<void>((*pipeline)->tryForwardUpdate(session));
            // A host must not admit a new visual Frame while the retained update is waiting.
            if (!(*pipeline)->hasPendingUpdate() && !session.hasPendingSubmit())
                break;
        }
        if (channel->requests.tryAcquireRead())
            updates += channel->requests.currentRead().kind == render::ERenderProgramKind::StateUpdate;
        assert(updates == 1);
        assert(!(*pipeline)->hasPendingUpdate());
        assert((*pipeline)->tryForwardUpdate(session) == scene::ERenderForwardResult::NO_UPDATE);
    }

    class Probe final : public lux::object::Object<Probe, lux::ui::Pane>
    {
    public:
        Probe(lux::ui::UISession& session, const char* id)
            : Object(session.dispatcherRef(), lux::ui::PaneId{id}, lux::ui::PaneTypeId{id}, id) {}
        lux::ui::ViewportResult result;
        unsigned draws{};
    private:
        void draw(lux::ui::Frame& frame, lux::ui::PaneDrawContext&) override
        {
            result = viewport_.draw(frame, {lux::ui::TextureHandle{1}});
            ++draws;
        }
        lux::ui::ViewportElement viewport_;
    };

    void layoutAndUnifiedInput()
    {
        using namespace lux;
        lux::ui::UISession session;
        Probe left{session, "left"}, center{session, "center"}, right{session, "right"};
        Probe bottom{session, "bottom"}, toolbar{session, "toolbar"};
        auto a = session.registerPane(left), b = session.registerPane(center), c = session.registerPane(right);
        auto d = session.registerPane(bottom), e = session.registerPane(toolbar);
        assert(a && b && c && d && e);
        session.setSplitLayout({"left", "center", "right", "bottom", 240, 300, 180, "toolbar"});
        const auto draw = [&](float width, float height) {
            auto frame = session.beginFrame({{width, height}, 0.016F, {1, 1}});
            frame.drawPanes();
            frame.finish();
        };
        draw(1600, 900);
        draw(1600, 900);
        assert(center.result.content_origin.x > left.result.content_origin.x);
        assert(center.result.content_origin.y >= 38);
        assert(right.result.content_origin.x > center.result.content_origin.x + center.result.size.width);
        const auto normal_width = center.result.size.width;
        right.setVisible(false);
        draw(1600, 900);
        assert(center.result.size.width > normal_width);
        right.setVisible(true);
        draw(1600, 900);
        assert(center.result.size.width == normal_width);
        const auto right_draws = right.draws;
        draw(800, 600);
        assert(right.draws == right_draws);
        draw(1600, 900);
        assert(session.requestFocus(lux::ui::PaneIdView{"center"}));
        draw(1600, 900);
        session.feedInput(lux::ui::UiPointerMove{{center.result.content_origin.x + 40,
            center.result.content_origin.y + 40}});
        draw(1600, 900);
        session.feedInput(lux::ui::UiPointerWheel{{0, 2}});
        {
            auto frame = session.beginFrame({{1600, 900}, 0.016F, {1, 1}});
            frame.drawPanes();
            editor::workbench::SceneCamera wheel_camera;
            const auto before = wheel_camera.position();
            assert(session.inputSnapshot().wheel.y == 2);
            wheel_camera.update(session.inputSnapshot(), center.result, 0.016);
            assert((wheel_camera.position() - before).norm() > 0);
            frame.finish();
        }
        session.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::RIGHT, true});
        session.feedInput(lux::ui::UiKey{lux::ui::EKey::W, true});
        draw(1600, 900);
        editor::workbench::SceneCamera camera;
        const auto initial = camera.position();
        camera.update(session.inputSnapshot(), center.result, 0.1);
        assert(camera.captured());
        draw(1600, 900);
        camera.update(session.inputSnapshot(), center.result, 0.1);
        assert((camera.position() - initial).norm() > 0);
        session.feedInput(lux::ui::UiWindowFocus{false});
        draw(1600, 900);
        camera.update(session.inputSnapshot(), center.result, 0.1);
        assert(!camera.captured());
    }
}

int main()
{
    using namespace lux;
    const editor::EditorSceneHandle scene{1, 1};
    const auto entity = simulation::ecs::Entity{7};
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = 1;
    const asset::AssetId mesh{bytes};
    bytes[0] = 2;
    const asset::AssetId material{bytes};
    const auto accepts = [&](editor::EditorSceneHandle current_scene, simulation::ecs::Entity current_entity,
        asset::AssetId current_mesh, asset::AssetId current_material, std::uint64_t serial) {
        return editor::workbench::detail::acceptsResource(scene, entity, mesh, material, 3,
            current_scene, current_entity, current_mesh, current_material, serial);
    };
    assert(accepts(scene, entity, mesh, material, 3));
    assert(!accepts({1, 2}, entity, mesh, material, 3));
    assert(!accepts(scene, simulation::ecs::Entity{(std::uint64_t{1} << 32) | 7}, mesh, material, 3));
    assert(!accepts(scene, entity, material, material, 3));
    assert(!accepts(scene, entity, mesh, mesh, 3));
    assert(!accepts(scene, entity, mesh, material, 4));
    pendingStateSurvivesFrameBackpressure();
    layoutAndUnifiedInput();
}
