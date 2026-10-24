#include <lux/engine/editor/ModelPlacement.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneTransformGizmo.hpp>
#include <lux/engine/editor/SceneViewportPane.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/math/Picking.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/ImageElement.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Root.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace lux::editor
{
    namespace
    {
        struct CameraSnapshot final
        {
            scene::Camera camera;
            Eigen::Affine3d transform;
            double aspect;
        };

        std::optional<CameraSnapshot> viewportCamera(
            scene::SceneRuntime& runtime,
            const SceneViewportStatus& viewport
        ) noexcept
        {
            auto borrowed = std::as_const(runtime).borrowInstance(viewport.instance);
            if (!borrowed)
            {
                return {};
            }
            const auto& registry = borrowed->get();
            const bool available = registry.valid(viewport.camera) &&
                                   registry.all_of<scene::Camera, simulation::ecs::WorldTransform3D>(viewport.camera) &&
                                   viewport.description.height != 0;
            if (!available)
            {
                return {};
            }
            return CameraSnapshot{
                registry.get<scene::Camera>(viewport.camera),
                registry.get<simulation::ecs::WorldTransform3D>(viewport.camera).value,
                double(viewport.description.width) / viewport.description.height
            };
        }

        std::optional<math::Ray3d> viewportRay(const CameraSnapshot& camera, ui::Point pointer, ui::Size size) noexcept
        {
            if (size.width <= 0 || size.height <= 0)
            {
                return {};
            }
            const auto projection = scene::cameraProjection(camera.camera, camera.aspect);
            const auto view = scene::cameraView(camera.transform, Eigen::Vector3d::Zero());
            if (!projection || !view)
            {
                return {};
            }
            const Eigen::Matrix4d inverse = (*projection * *view).inverse();
            math::Ray3d ray;
            math::screenToRay(
                double(pointer.x),
                double(pointer.y),
                double(size.width),
                double(size.height),
                inverse,
                ray
            );
            return ray;
        }

        std::optional<Eigen::Vector3d> groundPosition(
            const CameraSnapshot& camera,
            ui::Point pointer,
            ui::Size size
        ) noexcept
        {
            const auto ray = viewportRay(camera, pointer, size);
            if (!ray)
            {
                return {};
            }
            const double distance = std::abs(ray->direction.y()) > 1e-8 ? -ray->origin.y() / ray->direction.y() : -1;
            if (!std::isfinite(distance) || distance < 0)
            {
                return {};
            }
            return Eigen::Vector3d{ray->origin + distance * ray->direction};
        }
    } // namespace

    struct SceneViewportPane::Impl final
    {
        class ViewportImage final : public ui::ImageElement
        {
        public:
            explicit ViewportImage(SceneViewportPane& pane) noexcept : pane_(pane) {}

            struct Drop final
            {
                ModelAssetReference asset;
                asset::AssetId document;
                std::uint64_t based_on{};
                ui::Point pointer;
                ui::Size size;
            };

            asset::AssetId document;
            std::uint64_t revision{};
            std::optional<Drop> drop;
            std::optional<Drop> pick;
            SceneTransformGizmo gizmo;

        private:
            void geometryChanged() noexcept override
            {
                pane_.root().deferChange(
                    pane_,
                    [](object::LuxObject& object) noexcept
                    { static_cast<SceneViewportPane&>(object).applyViewportExtent(); }
                );
            }

            SceneViewportPane& pane_;

            void draw() noexcept override
            {
                ImageElement::draw();
                const auto& input = interaction();
                const bool gizmo_active = gizmo.draw(input.content_origin, input.size);
                if (input.left_clicked && !input.drop && !gizmo_active)
                {
                    pick = Drop{{}, document, revision, input.local_pointer, input.size};
                }
                const bool accepted = input.drop && input.drop->type == ui::PayloadTypeIdView{kModelAssetPayload} &&
                                      input.drop->bytes.size() == sizeof(ModelAssetReference);
                if (!accepted)
                {
                    return;
                }
                ModelAssetReference copied;
                std::memcpy(&copied, input.drop->bytes.data(), sizeof(copied));
                drop = Drop{copied, document, revision, input.local_pointer, input.size};
            }
        };

        ProjectScenes& scenes;
        ProjectAssets& assets;
        scene::SceneRuntime& runtime;
        ui::Layout layout;
        ui::Choice documents{{}}, cameras{{{0, "Editor camera"}, {1, "Primary Scene camera"}}};
        ui::CheckBox play{"Observe Play instance"};
        ui::Layout position{ui::ELayoutType::HORIZONTAL};
        ui::NumericEdit x{0.0}, y{2.0}, z{8.0}, yaw{0.0}, pitch{-12.0};
        ui::Label status;
        ViewportImage image;
        std::string editing_status;
        std::vector<asset::AssetId> document_ids;
        std::vector<world::WorldObjectId> camera_objects;
        std::vector<object::Connection> connections;
        object::Connection document_changed;
        SceneViewportId viewport;
        asset::AssetId document;
        std::uint32_t width{640}, height{360};
        bool cameras_dirty{true}, camera_controls_dirty{true};
        bool observing_play{};
        bool dragging{};
        std::optional<bool> end_drag;
        std::optional<SceneEdit> pending_drop;
        std::uint64_t drag_revision{};
        Eigen::Vector3d last_translation{Eigen::Vector3d::Zero()};

        Impl(
            SceneViewportPane& pane,
            ProjectScenes& scenes,
            ProjectAssets& assets,
            scene::SceneRuntime& runtime
        ) noexcept
            : scenes(scenes), assets(assets), runtime(runtime), image(pane)
        {
            const auto add = [](auto& parent, auto& child)
            {
                if (!parent.addElement(child))
                {
                    std::terminate();
                }
            };
            for (auto* control : {&x, &y, &z, &yaw, &pitch})
            {
                add(position, *control);
            }
            add(layout, documents);
            add(layout, play);
            add(layout, cameras);
            add(layout, position);
            add(layout, status);
            add(layout, image);
            image.setMinimumSize({64, 64});
            image.setSize({640, 360});
            image.setStretch({1, 1});
            documents.setStretch({1, 0});
            cameras.setStretch({1, 0});
            position.setStretch({1, 0});
            status.setStretch({1, 0});
        }
    };

    SceneViewportPane::SceneViewportPane(
        ProjectScenes& scenes,
        ProjectAssets& assets,
        scene::SceneRuntime& runtime,
        std::string title
    )
        : Pane(std::move(title)), impl_(std::make_unique<Impl>(*this, scenes, assets, runtime))
    {
        if (!addElement(impl_->layout))
        {
            std::terminate();
        }
        const auto retain = [&](auto connection)
        {
            if (!connection)
            {
                std::terminate();
            }
            impl_->connections.push_back(std::move(*connection));
        };
        retain(connect(&scenes, &ProjectScenes::opened, this, [this](asset::AssetId) noexcept { refreshDocuments(); }));
        retain(connect(&scenes, &ProjectScenes::changed, this, [this]() noexcept { refreshDocuments(); }));
        retain(
            connect(&impl_->documents, &ui::Choice::edited, this, [this](ui::EditResult) noexcept { selectDocument(); })
        );
        retain(connect(&impl_->cameras, &ui::Choice::edited, this, [this](ui::EditResult) noexcept { updateCamera(); })
        );
        retain(connect(&impl_->play, &ui::CheckBox::edited, this, [this](ui::EditResult) noexcept { selectDocument(); })
        );
        for (auto* control : {&impl_->x, &impl_->y, &impl_->z, &impl_->yaw, &impl_->pitch})
        {
            retain(connect(control, &ui::NumericEdit::edited, this, [this](ui::EditResult) noexcept { updateCamera(); })
            );
        }
        updateCamera();
        refreshDocuments();
    }

    SceneViewportPane::~SceneViewportPane() noexcept
    {
        if (impl_->dragging)
        {
            if (auto* document = impl_->scenes.find(impl_->document))
            {
                static_cast<void>(document->finishPreview(true)); // Keep the last adopted visible value.
            }
        }
        impl_->image.setImage({});
        impl_->scenes.closeViewport(impl_->viewport);
    }

    void SceneViewportPane::refreshDocuments() noexcept
    {
        const auto previous = impl_->document;
        impl_->document_ids = impl_->scenes.documents();
        std::vector<ui::ChoiceOption> options;
        for (const auto id : impl_->document_ids)
        {
            const auto* document = impl_->scenes.find(id);
            options.push_back(
                {static_cast<std::int64_t>(options.size()), std::string{document->baseline().world->data().name()}}
            );
        }
        impl_->documents.setOptions(std::move(options));
        const auto found = std::ranges::find(impl_->document_ids, previous);
        if (found != impl_->document_ids.end())
        {
            impl_->documents.setValue(found - impl_->document_ids.begin());
            return;
        }
        impl_->documents.setValue(0);
        selectDocument();
    }

    void SceneViewportPane::selectDocument() noexcept
    {
        if (impl_->dragging)
        {
            if (auto* old = impl_->scenes.find(impl_->document))
            {
                static_cast<void>(old->finishPreview(true));
            }
            impl_->dragging = false;
            impl_->end_drag.reset();
            impl_->image.gizmo.cancel();
        }
        impl_->image.setImage({});
        impl_->pending_drop.reset();
        impl_->scenes.closeViewport(impl_->viewport);
        impl_->viewport = {};
        impl_->document = {};
        impl_->document_changed = {};
        const auto index = static_cast<std::size_t>(impl_->documents.value());
        if (index >= impl_->document_ids.size())
        {
            impl_->status.setText("Open a Scene to observe it");
            return;
        }
        impl_->document = impl_->document_ids[index];
        static_cast<void>(impl_->scenes.selectDocument(impl_->document));
        auto* document = impl_->scenes.find(impl_->document);
        auto connected =
            connect(document, &SceneDocument::changed, this, [this]() noexcept { impl_->cameras_dirty = true; });
        if (!connected)
        {
            std::terminate();
        }
        impl_->document_changed = std::move(*connected);
        impl_->cameras.setValue(0);
        updateCamera();
        impl_->observing_play = impl_->play.value();
        auto viewport = impl_->scenes.addViewport(impl_->document, impl_->width, impl_->height, impl_->observing_play);
        if (!viewport)
        {
            impl_->status.setText(describeProjectSceneFailure(viewport.error()));
            return;
        }
        impl_->viewport = *viewport;
        updateCamera();
        impl_->cameras_dirty = true;
    }

    void SceneViewportPane::refreshCameras() noexcept
    {
        auto* document = impl_->scenes.find(impl_->document);
        if (!document)
        {
            return;
        }
        const auto instance =
            impl_->observing_play ? impl_->scenes.playInstance(impl_->document) : document->instance();
        auto registry = std::as_const(impl_->runtime).borrowInstance(instance);
        if (!registry)
        {
            return; // A previous simulation publication still owns its mutation interval.
        }
        impl_->camera_objects.clear();
        std::vector<ui::ChoiceOption> options{{0, "Editor camera"}, {1, "Primary Scene camera"}};
        const auto& entities = registry->get();
        const auto* residency = entities.ctx().find<scene::WorldResidency>();
        const auto* identities = residency ? &residency->identities() : nullptr;
        for (auto entity : entities.view<const scene::Camera>())
        {
            const auto object = identities ? identities->object(entity) : world::WorldObjectId{};
            if (object.valid())
            {
                options.push_back(
                    {static_cast<std::int64_t>(options.size()), "Camera " + uuids::to_string(object.value)}
                );
                impl_->camera_objects.push_back(object);
            }
        }
        impl_->cameras.setOptions(std::move(options));
        impl_->cameras_dirty = false;
    }

    void SceneViewportPane::updateCamera() noexcept
    {
        const auto selected = impl_->cameras.value();
        ViewportCameraSelection selection;
        if (selected == 1)
        {
            selection.kind = EViewportCamera::PRIMARY;
        }
        else if (selected >= 2 && static_cast<std::size_t>(selected - 2) < impl_->camera_objects.size())
        {
            selection = {EViewportCamera::SCENE, impl_->camera_objects[selected - 2]};
        }
        static_cast<void>(impl_->scenes.selectViewportCamera(impl_->viewport, selection));
        if (selection.kind == EViewportCamera::EDITOR)
        {
            simulation::ecs::Transform3D pose;
            pose.translation = Eigen::Vector3d{
                std::get<double>(impl_->x.value()),
                std::get<double>(impl_->y.value()),
                std::get<double>(impl_->z.value())
            };
            constexpr double radians = 3.14159265358979323846 / 180.0;
            pose.rotation =
                Eigen::AngleAxisd(std::get<double>(impl_->yaw.value()) * radians, Eigen::Vector3d::UnitY()) *
                Eigen::AngleAxisd(std::get<double>(impl_->pitch.value()) * radians, Eigen::Vector3d::UnitX());
            impl_->camera_controls_dirty = !impl_->scenes.setViewportCameraTransform(impl_->viewport, pose);
        }
        else
        {
            impl_->camera_controls_dirty = false;
        }
    }

    void SceneViewportPane::update() noexcept
    {
        if (impl_->observing_play &&
            (!impl_->scenes.playInstance(impl_->document).valid() || impl_->scenes.stoppingPlay(impl_->document)))
        {
            impl_->play.setValue(false);
            selectDocument();
        }
        placeDroppedModel();
        pickObject();
        updateGizmo();
        impl_->image.document = impl_->document;
        if (const auto* document = impl_->scenes.find(impl_->document))
        {
            impl_->image.revision = document->revision();
        }
        if (impl_->cameras_dirty)
        {
            refreshCameras();
        }
        if (impl_->viewport.isNull())
        {
            return;
        }
        if (impl_->camera_controls_dirty)
        {
            updateCamera();
        }
        const auto status = impl_->scenes.viewport(impl_->viewport);
        impl_->image.setImage(status.image);
        impl_->image.setImageExtent({float(status.description.width), float(status.description.height)});
        impl_->status.setText(
            status.error.type ? error::format(status.error)
            : status.ready    ? impl_->editing_status
                              : "Preparing viewport..."
        );
    }

    void SceneViewportPane::applyViewportExtent() noexcept
    {
        const auto& geometry = impl_->image.interaction();
        const double width = std::ceil(double(geometry.available_size.width) * geometry.framebuffer_scale.x);
        const double height = std::ceil(double(geometry.available_size.height) * geometry.framebuffer_scale.y);
        const bool invalid_extent = !std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0 ||
                                    width > UINT32_MAX || height > UINT32_MAX;
        if (invalid_extent)
        {
            impl_->editing_status = "Viewport extent is outside the supported pixel range";
            return;
        }
        impl_->width = static_cast<std::uint32_t>(width);
        impl_->height = static_cast<std::uint32_t>(height);
        if (!impl_->viewport.isNull())
        {
            static_cast<void>(impl_->scenes.updateViewport(impl_->viewport, impl_->width, impl_->height));
        }
    }

    void SceneViewportPane::updateGizmo() noexcept
    {
        auto& state = *impl_;
        auto& gizmo = state.image.gizmo;
        auto* document = state.scenes.find(state.document);
        auto intent = gizmo.takeEdit();
        if (!document || state.observing_play || document->readOnly())
        {
            gizmo.hide();
            gizmo.cancel();
            state.dragging = false;
            return;
        }
        if (state.end_drag)
        {
            if (!document->finishPreview(*state.end_drag))
            {
                gizmo.hide();
                return;
            }
            state.end_drag.reset();
            state.dragging = false;
            gizmo.cancel();
            intent.reset();
        }
        if (intent)
        {
            if (intent->begin)
            {
                state.drag_revision = intent->based_on;
                state.dragging = true;
                auto value =
                    document->component(intent->object, simulation::ecs::componentSchemaId("lux.ecs.Transform3D"));
                if (value)
                {
                    state.last_translation =
                        static_cast<const simulation::ecs::Transform3D*>(value->data())->translation;
                }
            }
            SceneEditResult<void> result;
            if (!intent->cancel && state.last_translation != intent->value.translation)
            {
                auto edit = document->prepareComponentEdit(intent->object, state.drag_revision, intent->value);
                result = edit ? document->preview(*edit) : SceneEditResult<void>{cxx::unexpected(edit.error())};
                if (result)
                {
                    state.drag_revision = document->revision();
                    state.last_translation = intent->value.translation;
                }
            }
            if (intent->finish || !result)
            {
                const auto finished = document->finishPreview(!intent->cancel);
                if (!finished)
                {
                    // Structural publication is still pending. Keep the exact end intent for the next safe point.
                    state.end_drag = !intent->cancel;
                    gizmo.hide();
                    state.editing_status = "Waiting for transform publication before ending the drag";
                    return;
                }
                else
                {
                    state.dragging = false;
                    gizmo.cancel();
                }
            }
            if (!result)
            {
                state.editing_status = "Transform draft rejected; the adopted value is unchanged";
            }
        }
        const bool unavailable = !document->selection().valid() || (document->previewing() && !state.dragging);
        if (unavailable)
        {
            gizmo.hide();
            return;
        }
        auto borrowed = std::as_const(state.runtime).borrowInstance(document->instance());
        if (!borrowed)
        {
            gizmo.hide();
            return;
        }
        const auto& registry = borrowed->get();
        const auto* residency = registry.ctx().find<scene::WorldResidency>();
        const auto* identities = residency ? &residency->identities() : nullptr;
        if (!identities)
        {
            gizmo.hide();
            return;
        }
        const auto entity = identities->entity(document->selection());
        const bool has_transform =
            registry.valid(entity) &&
            registry.all_of<simulation::ecs::Transform3D, simulation::ecs::WorldTransform3D>(entity);
        if (!has_transform)
        {
            gizmo.hide();
            return;
        }
        Eigen::Matrix3d inverse_parent = Eigen::Matrix3d::Identity();
        if (const auto* parent = registry.try_get<const simulation::ecs::Parent>(entity))
        {
            if (const auto* transform = registry.try_get<const simulation::ecs::WorldTransform3D>(parent->entity))
            {
                inverse_parent = transform->value.linear().inverse();
            }
        }
        const auto camera = viewportCamera(state.runtime, state.scenes.viewport(state.viewport));
        if (!camera || !inverse_parent.allFinite())
        {
            gizmo.hide();
            return;
        }
        const auto projection = scene::cameraProjection(camera->camera, camera->aspect);
        const auto view = scene::cameraView(camera->transform, Eigen::Vector3d::Zero());
        if (!projection || !view)
        {
            gizmo.hide();
            return;
        }
        const auto& world = registry.get<simulation::ecs::WorldTransform3D>(entity).value;
        const double length = std::max(0.01, (world.translation() - camera->transform.translation()).norm() * 0.1);
        gizmo.prepare(
            document->selection(),
            document->revision(),
            registry.get<simulation::ecs::Transform3D>(entity),
            world,
            inverse_parent,
            *projection * *view,
            length
        );
    }

    void SceneViewportPane::pickObject() noexcept
    {
        const auto click = std::exchange(impl_->image.pick, {});
        if (!click || impl_->observing_play)
        {
            return;
        }
        auto* document = impl_->scenes.find(click->document);
        if (!document || click->document != impl_->document || click->based_on != document->revision())
        {
            return;
        }
        const auto camera = viewportCamera(impl_->runtime, impl_->scenes.viewport(impl_->viewport));
        const auto ray = camera ? viewportRay(*camera, click->pointer, click->size) : std::nullopt;
        if (!ray)
        {
            return;
        }
        auto borrowed = std::as_const(impl_->runtime).borrowInstance(document->instance());
        if (!borrowed)
        {
            return;
        }
        const auto& registry = borrowed->get();
        const auto* query = registry.ctx().find<scene::MeshQuery>();
        if (!query)
        {
            impl_->editing_status = "This Scene has no MeshQuery system; select objects in Hierarchy";
            return;
        }
        scene::RayHit3D hit;
        const auto found = query->raycastNearest(*ray, 1.0e9, hit);
        if (!found)
        {
            impl_->editing_status = "Mesh query is not ready or its asset failed";
            return;
        }
        const auto* residency = registry.ctx().find<scene::WorldResidency>();
        const auto* identities = residency ? &residency->identities() : nullptr;
        if (identities)
        {
            static_cast<void>(impl_->scenes.selectDocument(document->id()));
            static_cast<void>(document->select(*found ? identities->object(hit.entity) : world::WorldObjectId{}));
        }
    }

    void SceneViewportPane::placeDroppedModel() noexcept
    {
        if (impl_->pending_drop)
        {
            auto* document = impl_->scenes.find(impl_->document);
            if (!document)
            {
                impl_->pending_drop.reset();
                return;
            }
            auto applied = document->apply(*impl_->pending_drop);
            if (!applied && isSceneEditBusy(applied.error()))
            {
                impl_->editing_status = "Waiting for the Scene edit safe point";
                return;
            }
            if (applied)
            {
                const auto& created = std::get<SceneCreateObjects>(impl_->pending_drop->operation).objects;
                if (!created.empty())
                {
                    static_cast<void>(document->select(created.front().id));
                }
            }
            impl_->pending_drop.reset();
            impl_->editing_status = applied ? "Model placed; GPU assets load asynchronously" : "Model edit rejected";
            return;
        }
        if (!impl_->image.drop)
        {
            return;
        }
        const auto drop = std::exchange(impl_->image.drop, {});
        auto* document = impl_->scenes.find(drop->document);
        const asset::AssetId id{drop->asset.id};
        const auto record = std::ranges::find(impl_->assets.records(), id, &ProjectAsset::id);
        const bool stale = !document || drop->document != impl_->document || document->revision() != drop->based_on ||
                           record == impl_->assets.records().end() || record->revision != drop->asset.revision;
        if (stale)
        {
            impl_->editing_status = "Scene or asset changed during drag; drop again";
            return;
        }
        const auto* model = impl_->assets.model(id);
        if (!model)
        {
            impl_->editing_status = "Model has not loaded";
            return;
        }
        if (document->baseline().world->data().partitioner().id.name != "lux.spatial.builtin.single")
        {
            impl_->editing_status = "This indexed World needs an explicit placement partition";
            return;
        }
        const auto camera = viewportCamera(impl_->runtime, impl_->scenes.viewport(impl_->viewport));
        const auto position = camera ? groundPosition(*camera, drop->pointer, drop->size) : std::nullopt;
        if (!position)
        {
            impl_->editing_status = "Point a valid camera at the ground plane to place a model";
            return;
        }
        // Initial placement is on world Y=0. Geometry picking remains a separate tool operation.
        Eigen::Affine3d placement = Eigen::Affine3d::Identity();
        placement.translation() = *position;
        auto edit = prepareModelPlacement(*document, *model, placement, {0});
        if (!edit)
        {
            impl_->editing_status = "Model transform or Scene schemas cannot represent this model";
            return;
        }
        edit->based_on = drop->based_on;
        impl_->pending_drop = std::move(*edit);
    }
} // namespace lux::editor
