#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneViewportPane.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/ImageElement.hpp>
#include <lux/engine/ui/Layout.hpp>

#include <algorithm>
#include <cmath>

namespace lux::editor
{
    struct SceneViewportPane::Impl final
    {
        ProjectScenes& scenes;
        scene::SceneRuntime& runtime;
        ui::Layout layout;
        ui::Choice documents{{}}, cameras{{{0, "Editor camera"}, {1, "Primary Scene camera"}}};
        ui::Layout position{ui::ELayoutType::HORIZONTAL};
        ui::NumericEdit x{0.0}, y{2.0}, z{8.0}, yaw{0.0}, pitch{-12.0};
        ui::Label status;
        ui::ImageElement image;
        std::vector<asset::AssetId> document_ids;
        std::vector<simulation::ecs::Entity> camera_entities;
        std::vector<object::Connection> connections;
        object::Connection document_changed;
        SceneViewportId viewport;
        asset::AssetId document;
        scene::VSceneCameraSource camera{scene::SceneCameraInput{}};
        std::uint32_t width{640}, height{360};
        bool cameras_dirty{true};

        Impl(ProjectScenes& scenes, scene::SceneRuntime& runtime) noexcept : scenes(scenes), runtime(runtime)
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

    SceneViewportPane::SceneViewportPane(ProjectScenes& scenes, scene::SceneRuntime& runtime, std::string title)
        : Pane(std::move(title)), impl_(std::make_unique<Impl>(scenes, runtime))
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
        impl_->image.setImage({});
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
        auto viewport = impl_->scenes.addViewport(impl_->document, impl_->camera, impl_->width, impl_->height);
        if (!viewport)
        {
            impl_->status.setText(describeProjectSceneFailure(viewport.error()));
            return;
        }
        impl_->viewport = *viewport;
        impl_->cameras_dirty = true;
    }

    void SceneViewportPane::refreshCameras() noexcept
    {
        auto* document = impl_->scenes.find(impl_->document);
        if (!document)
        {
            return;
        }
        auto registry = std::as_const(impl_->runtime).borrowInstance(document->instance());
        if (!registry)
        {
            return; // A previous simulation publication still owns its mutation interval.
        }
        impl_->camera_entities.clear();
        std::vector<ui::ChoiceOption> options{{0, "Editor camera"}, {1, "Primary Scene camera"}};
        const auto& entities = registry->get();
        const auto* identities = entities.ctx().find<simulation::ecs::WorldEntityMap>();
        for (auto entity : entities.view<const scene::Camera>())
        {
            const auto object = identities ? identities->object(entity) : world::WorldObjectId{};
            options.push_back({static_cast<std::int64_t>(options.size()), "Camera " + uuids::to_string(object.value)});
            impl_->camera_entities.push_back(entity);
        }
        impl_->cameras.setOptions(std::move(options));
        impl_->cameras_dirty = false;
    }

    void SceneViewportPane::updateCamera() noexcept
    {
        const auto selected = impl_->cameras.value();
        if (selected == 0)
        {
            scene::SceneCameraInput input;
            input.transform.translation() = Eigen::Vector3d{
                std::get<double>(impl_->x.value()),
                std::get<double>(impl_->y.value()),
                std::get<double>(impl_->z.value())
            };
            constexpr double radians = 3.14159265358979323846 / 180.0;
            input.transform.linear() =
                (Eigen::AngleAxisd(std::get<double>(impl_->yaw.value()) * radians, Eigen::Vector3d::UnitY()) *
                 Eigen::AngleAxisd(std::get<double>(impl_->pitch.value()) * radians, Eigen::Vector3d::UnitX()))
                    .toRotationMatrix();
            impl_->camera = std::move(input);
        }
        else if (selected == 1)
        {
            impl_->camera = scene::PrimarySceneCamera{};
        }
        else if (static_cast<std::size_t>(selected - 2) < impl_->camera_entities.size())
        {
            impl_->camera = scene::SceneEntityCamera{impl_->camera_entities[selected - 2]};
        }
        static_cast<void>(impl_->scenes.updateViewport(impl_->viewport, impl_->camera, impl_->width, impl_->height));
    }

    void SceneViewportPane::update() noexcept
    {
        if (impl_->cameras_dirty)
        {
            refreshCameras();
        }
        if (impl_->viewport.isNull())
        {
            return;
        }
        const auto size = impl_->image.displayedSize();
        const auto width = static_cast<std::uint32_t>(std::max(1.0F, std::floor(size.width)));
        const auto height = static_cast<std::uint32_t>(std::max(1.0F, std::floor(size.height)));
        if (size.width > 0 && size.height > 0 && (width != impl_->width || height != impl_->height))
        {
            impl_->width = width;
            impl_->height = height;
            static_cast<void>(impl_->scenes.updateViewport(impl_->viewport, impl_->camera, width, height));
        }
        const auto status = impl_->scenes.viewport(impl_->viewport);
        impl_->image.setImage(status.ready ? status.image : rdesc::UiImageToken{});
        impl_->status.setText(
            status.error.type ? error::format(status.error)
            : status.ready    ? ""
                              : "Preparing viewport..."
        );
    }
} // namespace lux::editor
