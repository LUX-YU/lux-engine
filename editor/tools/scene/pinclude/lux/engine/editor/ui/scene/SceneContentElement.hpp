#include <lux/engine/editor/CloseStatus.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#pragma once

#include <lux/engine/ui/Element.hpp>
#include <lux/engine/editor/ui/AssetActions.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/editor/ui/SpatialInteraction.hpp>
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>
#include <lux/engine/editor/ui/SceneElement.hpp>

namespace lux::editor::ui
{
    class SceneContentElement final : public lux::ui::Element
    {
    public:
        SceneContentElement(
            scene::SceneEditor::Impl&,
            lux::scene::SceneRuntime&,
            const std::shared_ptr<const lux::scene::ScenePackage>&,
            lux::scene::RenderResources&,
            std::string,
            EditorResult<void>&,
            std::span<const SpatialInteractionRegistration> = {}
        );
        void update() noexcept override;
        void requestClose() noexcept;
        void reopen() noexcept;
        void applyControl() noexcept;
        void command(object::EventView& event) noexcept { assets_.command(event); }
        CloseStatus closeStatus() const;

    private:
        void applyViewChange() noexcept;
        void draw() noexcept override;
        lux::ui::SizeHint sizeHintContent() noexcept override;
        lux::ui::SizeHint measureContent(float width) noexcept override;
        void arrangeContent() noexcept override;
        void drawDetails() noexcept;
        class Details final : public lux::ui::Element
        {
        public:
            explicit Details(SceneContentElement& owner);

        private:
            lux::ui::SizeHint sizeHintContent() noexcept override;
            void draw() noexcept override
            {
                owner_.drawDetails();
            }
            SceneContentElement& owner_;
        };
        lux::ui::Layout layout_;
        TAssetActions<scene::SceneEditor> assets_;
        lux::ui::Layout toolbar_;
        lux::ui::Button play_, pause_, resume_, step_, stop_;
        lux::ui::Label clock_;
        Details details_;
        std::array<object::Connection, 5> controls_;
        scene::SceneEditor::Impl& editor_;
        bool closing_{};
        enum class EControl : std::uint8_t
        {
            NONE,
            PLAY,
            PAUSE,
            RESUME,
            STEP,
            STOP
        };
        EControl control_{};
        scene::RunId control_run_;
        enum class EDetailAction : std::uint8_t
        {
            PROJECTION,
            CREATE_CAMERA,
            CANCEL,
            RETRY,
            DISCARD
        };
        struct DetailAction final
        {
            EDetailAction action;
            lux::simulation::ecs::Entity camera{lux::simulation::ecs::NullEntity};
            editing::StateId state;
            scene::ModelCreationId placement;
            std::uint32_t partition{};
            int projection{};
            lux::scene::SceneInstanceId scene_id;
        };
        std::optional<DetailAction> detail_action_;
        void applyDetailAction() noexcept;
        void drawPlacement();

        lux::scene::SceneRuntime& runtime_;
        const std::shared_ptr<const lux::scene::ScenePackage>& source_;
        lux::scene::RenderResources& resources_;
        std::unique_ptr<SpatialInteraction> spatial_;
        lux::simulation::ecs::Entity camera_{lux::simulation::ecs::NullEntity};
        lux::scene::SceneInstanceId camera_scene_;
        CameraMotion pending_motion_;
        bool navigation_pending_{};
        struct Pick final
        {
            lux::scene::SceneInstanceId instance;
            lux::math::Ray3d ray;
            lux::simulation::ecs::Entity camera{lux::simulation::ecs::NullEntity};
            std::uint64_t selection_revision{};
        };
        struct Create final
        {
            Pick location;
            AssetReference asset;
            lux::partition::PartitionOrdinal partition;
            double plane;
            editing::StateId base;
        };
        std::variant<std::monostate, Pick, Create> action_;
        double work_plane_height_{};
        std::unique_ptr<ui::SceneElement> viewport_;
        scene::RunId displayed_run_;
        lux::system::SystemInstanceId displayed_render_;
        std::optional<lux::system::SystemInstanceId> render_selection_;
        EditorResult<void> view_result_;
        bool reopen_{};
        lux::scene::RenderResourceId view() noexcept;
        EditorResult<lux::math::Ray3d> cameraRay(Eigen::Vector2d, Eigen::Vector2d) const;
        void remember(std::string_view, const lux::render::RendererFailure&);
        std::string status_;
        scene::ModelCreationId placement_;
        std::uint32_t partition_{};
        bool rotating_{}, panning_{}, closed_{};
    };
} // namespace lux::editor::ui
