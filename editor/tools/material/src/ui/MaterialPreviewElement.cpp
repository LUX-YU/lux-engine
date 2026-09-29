#include <lux/engine/editor/ui/material/MaterialPreviewElement.hpp>
#include <imgui.h>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>

namespace lux::editor::ui
{
    MaterialPreviewElement::MaterialPreviewElement(
        lux::ui::Element& parent,
        material::MaterialEditor::Impl& editor_,
        lux::scene::SceneRuntime& runtime,
        lux::scene::RenderResources& resources,
        lux::system::SystemInstanceId render_system
    )
        : lux::ui::Element(parent, lux::ui::ElementId{"preview"}), editor_(editor_), runtime_(runtime),
          resources_(resources), render_system_(render_system), layout_(*this, lux::ui::ElementId{"layout"}),
          status_(layout_, lux::ui::ElementId{"status"}), spatial_(spatialInteraction3D().create())
    {}

    void MaterialPreviewElement::applyViewChange() noexcept
    {
        const auto instance = editor_.previewInstance();
        if (!instance.valid())
            return;
        if (!viewport_ && visible())
        {
            const auto camera = editor_.previewCamera();
            auto opened = ui::SceneElement::create(
                layout_,
                lux::ui::ElementId{std::string(id().name()) + ".view"},
                runtime_,
                instance,
                resources_,
                render_system_,
                camera,
                {{640, 480}, lux::scene::SampledOutput{}}
            );
            if (!opened)
            {
                if (opened.error().code != lux::render::ERendererError::NOT_READY &&
                    opened.error().code != lux::render::ERendererError::BUSY)
                    error_ = "Preview view could not open";
                return;
            }
            viewport_ = std::move(*opened);
            camera_ = camera;
        }
    }

    void MaterialPreviewElement::update() noexcept
    {
        status_.setText(error_.empty() ? editor_.previewStatus() : error_);
        const auto instance = editor_.previewInstance();
        if ((!viewport_ && visible() && instance.valid()))
            root().deferChange(*this, [](object::LuxObject& target) noexcept {
                static_cast<MaterialPreviewElement&>(target).applyViewChange();
            });
        if (!instance.valid())
            return;
        const bool moved = motion_.angular_delta.squaredNorm() || motion_.pan_delta.squaredNorm() || motion_.dolly;
        const auto borrowed = std::as_const(runtime_).borrowInstance(instance);
        if (moved && borrowed && borrowed->get().valid(camera_))
        {
            const auto& pose = borrowed->get().get<lux::simulation::ecs::Transform3D>(camera_);
            const auto& camera = borrowed->get().get<lux::scene::Camera>(camera_);
            auto candidate = spatial_->navigate(pose, camera, motion_);
            if (candidate)
            {
                const auto pan = pose.rotation * Eigen::Vector3d{motion_.pan_delta.x(), motion_.pan_delta.y(), 0};
                const Eigen::Vector3d pivot = pivot_ + pan;
                const double radius =
                    std::clamp((pose.translation - pivot_).norm() * std::exp(-motion_.dolly * 0.12), 1.1, 100.0);
                candidate->transform.translation =
                    pivot + candidate->transform.rotation * Eigen::Vector3d{0, 0, radius};
                auto applied = editor_.navigatePreview(candidate->transform, candidate->camera);
                if (applied)
                {
                    pivot_ = pivot;
                    motion_ = {};
                }
            }
        }
    }

    lux::ui::SizeHint MaterialPreviewElement::sizeHintContent() noexcept
    {
        auto hint = layout_.sizeHint();
        hint.preferred = {320, 360};
        return hint;
    }
    lux::ui::SizeHint MaterialPreviewElement::measureContent(float width) noexcept
    {
        auto hint = layout_.measure(width);
        // A viewport's last pixel extent must not become the next preferred UI width.
        hint.preferred = {320, 360};
        return hint;
    }
    void MaterialPreviewElement::arrangeContent() noexcept
    {
        layout_.arrange({{}, rect().size});
    }

    void MaterialPreviewElement::draw() noexcept
    {
        drawChild(layout_);
        if (!viewport_)
            return;
        const auto& interaction = viewport_->image().interaction();
        const auto& io = ImGui::GetIO();
        const bool blocked =
            io.WantTextInput || io.AppFocusLost || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
        rotating_ = !blocked && (rotating_ || interaction.right_clicked) && ImGui::IsMouseDown(ImGuiMouseButton_Right);
        panning_ = !blocked && (panning_ || interaction.middle_clicked) && ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        if (rotating_)
            motion_.angular_delta += Eigen::Vector2d{-io.MouseDelta.x * 0.004, -io.MouseDelta.y * 0.004};
        if (panning_)
            motion_.pan_delta += Eigen::Vector2d{-io.MouseDelta.x * 0.01, io.MouseDelta.y * 0.01};
        if (!blocked && interaction.hovered)
            motion_.dolly += io.MouseWheel;
    }
}
