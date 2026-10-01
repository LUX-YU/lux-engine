#pragma once
#include <lux/engine/editor/scene/SceneView.hpp>
namespace lux::editor::scene
{
    class OutlinerView final : public lux::ui::Pane
    {
    public:
        OutlinerView(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            sessions::TSessionAccess<SceneSession>,
            VSceneViewBinding,
            std::optional<RunInspectAccess> = {},
            simulation::ecs::ComponentSchemaSet = {}
        );
        ~OutlinerView() noexcept override;
        OutlinerView(const OutlinerView&) = delete;
        OutlinerView& operator=(const OutlinerView&) = delete;
        OutlinerView(OutlinerView&&) = delete;
        OutlinerView& operator=(OutlinerView&&) = delete;
        [[nodiscard]] SceneViewResult<void> rebind(VSceneViewBinding);
        [[nodiscard]] SceneViewResult<void> select(VSceneSelectionTarget);
        [[nodiscard]] SceneViewResult<void> erase(std::span<const SceneObjectRef>);
        [[nodiscard]] SceneViewResult<void> reparent(SceneObjectRef, world::WorldObjectId);
        [[nodiscard]] SceneViewResult<void> createObject(
            world::WorldObjectId,
            partition::PartitionOrdinal,
            EObjectSpace
        );
        [[nodiscard]] std::span<const VSceneSelectionTarget> objects() const noexcept;
        [[nodiscard]] const SceneViewResult<void>& status() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        void update() noexcept override;
    };
    [[nodiscard]] SceneViewResult<views::DetachedView> makeOutlinerView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        sessions::TSessionAccess<SceneSession>,
        VSceneViewBinding,
        std::optional<RunInspectAccess> = {},
        simulation::ecs::ComponentSchemaSet = {}
    );
}
