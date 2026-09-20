#pragma once

#include "entity_checks.hpp"
#include <lux/engine/editor/gui/scene/SpatialViewport.hpp>
#include <lux/engine/editor/scene/FieldEdit.hpp>

#include <cassert>
#include <cstdio>

inline void checkSpatialEditing(lux::editor::scene::SceneEditor &editor)
{
    using namespace lux;
    using Camera = scene::Camera;
    using Transform = simulation::ecs::Transform3D;
    const auto before = editor.historyView()->history;
    const auto authors = entitySnapshot(editor);
    const auto camera = editor.viewportCamera();
    assert(camera && editor.objects().size() == authors.size());
    assert(std::ranges::find(authors, *camera) == authors.end());
    assert(!editor.eraseObjects(before.current, std::span(&*camera, 1)));
    if (editor.supportsHierarchy())
    {
        const auto target = editor.writeTarget(authors.front());
        assert(target && !editor.reparent(*target, *camera));
    }
    assert(editor.historyView()->history.current == before.current);

    const auto registration = editor::gui::spatialViewport3D();
    assert(registration.supports(editor));
    const auto viewport = registration.create();
    const auto center = viewport->ray(editor, *camera, {400, 300}, {800, 600});
    const auto high_dpi = viewport->ray(editor, *camera, {800, 600}, {1600, 1200});
    assert(center && high_dpi && center->direction.isApprox(high_dpi->direction));
    assert(!viewport->ray(editor, *camera, {1, 1}, {0, 600}));

    scene::RayHit3D hit;
    const math::Ray3d surface{{0, 1, 20}, {0, 0, -1}};
    const auto found = editor.raycastNearest(editor.instance(), surface, 100, hit);
    assert(found && *found && hit.entity == editor.objects().front().object.entity);
    const auto landing = viewport->creationPoint(editor, editor.instance(), surface, 0);
    assert(landing && landing->isApprox(hit.position));
    assert(editor.select({editor.instance(), hit.entity}));
    assert(editor.selection().object.entity == hit.entity);

    const auto plane = viewport->creationPoint(editor, editor.instance(), {{50, 10, 50}, {0, -1, 0}}, 2);
    assert(plane && plane->isApprox(Eigen::Vector3d{50, 2, 50}));
    assert(!viewport->creationPoint(editor, editor.instance(), {{50, 10, 50}, {1, 0, 0}}, 2));
    assert(!viewport->creationPoint(editor, editor.instance(), {{50, 10, 50}, {0, 1, 0}}, 2));

    auto projection = *static_cast<const Camera *>(editor.component(*camera, cxx::typeToken<Camera>()));
    const auto pose = *static_cast<const Transform *>(editor.component(*camera, cxx::typeToken<Transform>()));
    projection.projection = scene::OrthographicProjection{12, 0.1, 1000};
    assert(editor.navigateCamera(*camera, pose, projection));
    const auto a = viewport->ray(editor, *camera, {200, 300}, {800, 600});
    const auto b = viewport->ray(editor, *camera, {600, 300}, {800, 600});
    assert(a && b && a->direction.isApprox(b->direction) && !a->origin.isApprox(b->origin));
    assert(editor.historyView()->history.current == before.current);

    const auto selection_before_create = editor.selection();
    editor::scene::SelectionNotice selection_notice;
    auto connection = editor.observeScoped<editor::scene::SceneEditor::selectionChanged>(
        [&](const editor::scene::SelectionNotice &notice) noexcept { selection_notice = notice; });
    auto created = editor.createCameraFromView(*camera, before.current, partition::PartitionOrdinal{0});
    assert(created && editor.objects().size() == authors.size() + 1);
    assert(editor.selection().revision > selection_before_create.revision);
    assert(selection_notice.object == editor.selection().object &&
           selection_notice.revision == editor.selection().revision);
    const auto creation_selection_revision = editor.selection().revision;
    const auto *value = static_cast<const Camera *>(editor.component(*created, cxx::typeToken<Camera>()));
    assert(value && !value->view.isValid() && !value->primary);
    assert(std::holds_alternative<scene::OrthographicProjection>(value->projection));
    assert(editor.historyView()->history.cursor == before.cursor + 1);
    assert(editor.undo() && !editor.component(*created, cxx::typeToken<Camera>()));
    assert(editor.selection().revision > creation_selection_revision);
    const auto undo_selection_revision = editor.selection().revision;
    assert(editor.objects().size() == authors.size() && editor.redo());
    assert(editor.selection().revision > undo_selection_revision);
    assert(selection_notice.object == editor.selection().object &&
           selection_notice.revision == editor.selection().revision);
    const auto restored = newEntities(editor, authors);
    assert(restored.size() == 1 && restored.front() != *created);
    assert(!editor.select(*created));
    assert(editor.component(restored.front(), cxx::typeToken<Camera>()));
    std::puts("PASS spatial: Entity hit, surface/work-plane/rejection, DPI, orthographic rays, protected temporary "
              "camera, one camera Undo/Redo, stale generation");
}

inline void checkReopenedCameras(const lux::editor::scene::SceneEditor &editor)
{
    std::size_t count{}, orthographic{};
    for (const auto &row : editor.objects())
    {
        const auto *camera = static_cast<const lux::scene::Camera *>(
            editor.component(row.object, lux::cxx::typeToken<lux::scene::Camera>()));
        if (camera)
        {
            ++count;
            assert(!camera->view.isValid());
            if (const auto *projection = std::get_if<lux::scene::OrthographicProjection>(&camera->projection))
            {
                ++orthographic;
                assert(projection->vertical_extent == 12 && !camera->primary);
            }
        }
    }
    assert(count == 2 && orthographic == 1);
    std::puts("PASS camera source reopen: two author cameras, projection preserved, runtime View omitted, CameraMan "
              "excluded");
}

struct SpatialRunChecks final
{
    lux::editor::scene::RunId run;
    unsigned phase{};
    unsigned scenario{};

    bool poll(lux::editor::scene::SceneEditor &editor)
    {
        using namespace lux;
        using Camera = scene::Camera;
        if (phase == 0)
        {
            // First remove the only primary; then introduce two primaries.
            for (const auto &row : editor.objects())
            {
                const auto *camera =
                    static_cast<const Camera *>(editor.component(row.object, cxx::typeToken<Camera>()));
                if (camera && camera->primary == (scenario == 0))
                {
                    assert(editor.setField<Camera>(
                        *editor.writeTarget(row.object), "Camera.primary", "Primary",
                        [](auto &value) { return &value.primary; }, scenario != 0));
                    const auto started = editor.play();
                    assert(started);
                    run = *started;
                    phase = 1;
                    break;
                }
            }
            assert(phase == 1);
        }
        if (phase == 1)
        {
            const auto status = editor.runStatus();
            assert(status.result);
            if (status.state != editor::scene::ERunState::RUNNING)
            {
                return false;
            }
            const auto camera = editor.viewportCamera();
            assert(!camera && camera.error().domain == "camera.primary");
            assert(camera.error().reason ==
                   static_cast<std::uint64_t>(scenario == 0 ? scene::ECameraError::NO_PRIMARY_CAMERA
                                                            : scene::ECameraError::MULTIPLE_PRIMARY_CAMERAS));
            assert(editor.stopRun(run));
            phase = 2;
        }
        if (phase == 2)
        {
            const auto status = editor.runStatus();
            assert(status.result);
            if (status.state != editor::scene::ERunState::FINISHED)
            {
                return false;
            }
            assert(editor.undo());
            ++scenario;
            phase = 0;
            if (scenario == 2)
            {
                std::puts("PASS Run camera qualification: no primary and conflicting primaries reject exactly; Run "
                          "advances and closes; author history restored");
                return true;
            }
        }
        return false;
    }
};
