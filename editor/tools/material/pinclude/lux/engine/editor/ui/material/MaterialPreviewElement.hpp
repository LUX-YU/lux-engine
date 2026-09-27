#pragma once
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/editor/ui/SceneElement.hpp>
#include <lux/engine/editor/ui/SpatialInteraction.hpp>
#include <lux/engine/editor/material/MaterialEditorImpl.hpp>

namespace lux::editor::ui
{
    class MaterialPreviewElement final : public lux::ui::Element
    {
    public:
        MaterialPreviewElement(
            lux::ui::Element&,
            material::MaterialEditor::Impl&,
            lux::scene::SceneRuntime&,
            lux::scene::RenderResources&,
            lux::system::SystemInstanceId
        );
        void update() noexcept override;

    private:
        void applyViewChange() noexcept;
        void draw() noexcept override;
        lux::ui::SizeHint sizeHintContent() noexcept override;
        lux::ui::SizeHint measureContent(float width) noexcept override;
        void arrangeContent() noexcept override;
        material::MaterialEditor::Impl& editor_;
        lux::scene::SceneRuntime& runtime_;
        lux::scene::RenderResources& resources_;
        lux::system::SystemInstanceId render_system_;
        lux::ui::Layout layout_;
        lux::ui::Label status_;
        std::unique_ptr<ui::SceneElement> viewport_;
        std::unique_ptr<SpatialInteraction> spatial_;
        lux::simulation::ecs::Entity camera_{lux::simulation::ecs::NullEntity};
        CameraMotion motion_;
        Eigen::Vector3d pivot_{Eigen::Vector3d::Zero()};
        std::string error_;
        bool rotating_{}, panning_{};
    };
}
