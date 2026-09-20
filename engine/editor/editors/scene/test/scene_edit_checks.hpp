#pragma once
#include <cassert>
#include <cstdio>
#include <limits>
#include <lux/engine/editor/gui/asset/AssetReference.hpp>
#include <lux/engine/editor/gui/scene/ComponentBinding.hpp>
#include <lux/engine/editor/gui/scene/InspectorInteraction.hpp>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/ui/UISession.hpp>

inline void checkGeneratedTransformDragging(lux::editor::scene::SceneEditor &document)
{
    using namespace lux::editor;
    using Transform = lux::simulation::ecs::Transform3D;
    using World = lux::simulation::ecs::WorldTransform3D;
    lux::ui::UISession ui;

    struct ProbePane final : lux::object::Object<ProbePane, lux::ui::Pane>
    {
        scene::SceneEditor &document;
        gui::InspectorInteraction interaction;
        gui::ComponentBinding binding;
        lux::editor::scene::SceneEntityRef object;
        std::array<ImVec2, 3> centers{};

        ProbePane(lux::ui::UISession &ui, scene::SceneEditor &value)
            : Object(ui.dispatcherRef(), lux::ui::PaneId{"transform-probe"}, lux::ui::PaneTypeId{"test"}, "Transform"),
              document(value), interaction(value, "transform-probe")
        {
            for (auto &candidate : gui::firstPartyComponentBindings())
            {
                if (candidate.type == lux::cxx::typeToken<Transform>())
                {
                    binding = std::move(candidate);
                }
            }
            assert(binding.draw);
        }

        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
        {
            ImGui::GetIO().MouseDoubleClickTime = 0;
            auto table = frame.table({lux::ui::WidgetIdView{"transform"}, 2, false, false, false, 110});
            assert(table.visible());
            binding.draw(document, object, frame, interaction);
            // The generated three vector rows have identical geometry. The last
            // item is the Scale group; derive Translation hit points from it.
            const auto low = ImGui::GetItemRectMin();
            const auto high = ImGui::GetItemRectMax();
            const auto &style = ImGui::GetStyle();
            const float label = ImGui::CalcTextSize("X").x;
            const float width = (high.x - low.x - 3 * (label + style.ItemInnerSpacing.x) - 2 * style.ItemSpacing.x) / 3;
            const float y = (low.y + high.y) / 2 - 2 * (ImGui::GetFrameHeight() + 2 * style.CellPadding.y);
            for (std::size_t axis{}; axis < centers.size(); ++axis)
            {
                centers[axis] = {
                    low.x + width / 2 + axis * (width + label + style.ItemInnerSpacing.x + style.ItemSpacing.x), y};
            }
            assert(interaction.finishDraw());
            assert(!interaction.error[0]);
        }
    } pane(ui, document);

    auto registered = ui.registerPane(pane);
    assert(registered);
    const auto draw = [&]
    {
        auto frame = ui.beginFrame({{800, 600}, 1.0F / 60});
        frame.drawPanes();
        frame.finish();
        PollBudget budget;
        document.poll(budget);
    };
    for (std::size_t index{}; index < 3; ++index)
    {
        pane.object = document.objects()[index].object;
        pane.interaction.reset();
        assert(ui.requestFocus(pane.id().view()));
        draw();
        draw();
        for (std::size_t axis{}; axis < 3; ++axis)
        {
            const auto before = document.historyView()->history;
            const Eigen::Vector3d original =
                static_cast<const Transform *>(document.component(pane.object, lux::cxx::typeToken<Transform>()))
                    ->translation;
            const auto center = pane.centers[axis];
            ui.feedInput(lux::ui::UiPointerMove{{center.x, center.y}});
            draw();
            ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true});
            draw();
            assert(!pane.interaction.active());
            ui.feedInput(lux::ui::UiPointerMove{{center.x + 60, center.y}});
            draw();
            const Eigen::Vector3d live =
                static_cast<const Transform *>(document.component(pane.object, lux::cxx::typeToken<Transform>()))
                    ->translation;
            const Eigen::Vector3d world =
                static_cast<const World *>(document.component(pane.object, lux::cxx::typeToken<World>()))
                    ->value.translation();
            std::printf("generated-drag object=%zu axis=%zu before=%g live=%g world=%g\n", index, axis, original[axis],
                        live[axis], world[axis]);
            assert(live[axis] != original[axis] && world.isApprox(live, 1e-10));
            ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
            draw();
            draw();
            assert(!pane.interaction.active());
            assert(document.historyView()->history.cursor == before.cursor + 1);
            assert(document.undo());
            draw();
            const auto *restored =
                static_cast<const World *>(document.component(pane.object, lux::cxx::typeToken<World>()));
            assert(restored->value.translation().isApprox(original, 1e-10));
        }
    }
    std::puts("PASS generated Transform UI: three objects, XYZ drags, live derivation, one history entry and Undo");
}

inline void checkSceneEditing(lux::editor::scene::SceneEditor &document)
{
    using namespace lux::editor;
    using Transform = lux::simulation::ecs::Transform3D;
    const auto &catalog = document.project();
    assert(!catalog.catalog().empty());
    const auto &asset = catalog.catalog().front();
    auto reference = catalog.reference(asset.id);
    assert(catalog.resolveReference(reference, asset.magic));
    const auto payload = gui::decodeAssetReference(std::as_bytes(std::span(&reference, 1)));
    assert(payload && payload->asset == asset.id);
    assert(!gui::decodeAssetReference(std::as_bytes(std::span(&reference, 1)).first(1)));
    ++reference.project_instance;
    const auto foreign = catalog.resolveReference(reference, 0);
    assert(!foreign &&
           std::any_cast<EAssetReferenceError>(foreign.error().cause) == EAssetReferenceError::FOREIGN_PROJECT);
    reference = catalog.reference(asset.id);
    ++reference.catalog_revision;
    const auto stale_catalog = catalog.resolveReference(reference, 0);
    assert(!stale_catalog &&
           std::any_cast<EAssetReferenceError>(stale_catalog.error().cause) == EAssetReferenceError::STALE_CATALOG);
    reference = catalog.reference(asset.id);
    const auto wrong_type = catalog.resolveReference(reference, asset.magic + 1);
    assert(!wrong_type &&
           std::any_cast<EAssetReferenceError>(wrong_type.error().cause) == EAssetReferenceError::WRONG_TYPE);
    std::printf(
        "asset-catalog: rows=%zu revision=%llu exact foreign/stale/type rejection; malformed payload retained\n",
        catalog.catalog().size(), static_cast<unsigned long long>(catalog.catalogRevision()));
    auto row =
        std::ranges::find_if(document.objects(), [&](const auto &item)
                             { return document.component(item.object, lux::cxx::typeToken<Transform>()) != nullptr; });
    assert(row != document.objects().end());
    const auto object = row->object;
    const auto read = [&]() -> Eigen::Vector3d
    {
        return static_cast<const Transform *>(document.component(object, lux::cxx::typeToken<Transform>()))
            ->translation;
    };
    const auto field = [](auto &value) noexcept { return &value.translation; };
    const auto original = read();
    const Eigen::Vector3d first = original + Eigen::Vector3d{1, 2, 3};
    const Eigen::Vector3d second = original + Eigen::Vector3d{4, 5, 6};
    const auto initial = *document.historyView();
    const auto target = *document.writeTarget(object);
    std::size_t notices{};
    auto connection = document.observeScoped<scene::SceneEditor::componentChanged>(
        [&](const auto &notice) noexcept
        {
            assert(notice.object == object);
            const auto blocked = document.writeTarget(object);
            assert(!blocked && blocked.error().code == editing::EEditError::BUSY);
            assert(document.historyView()->undo == editing::EHistoryActionAvailability::BUSY);
            ++notices;
        });

    auto applied = document.setField<Transform>(target, "Transform3D.translation", "Translation", field, first);
    assert(applied && applied->effect == editing::EEditEffect::CHANGE && read() == first);
    const auto committed = *document.historyView();
    assert(committed.history.cursor == 1 && committed.history.revision.value == initial.history.revision.value + 1);
    auto stale = document.setField<Transform>(target, "Transform3D.translation", "Translation", field, second);
    assert(!stale && stale.error().code == editing::EEditError::STALE_BASE && read() == first);
    assert(document.historyView()->history.revision == committed.history.revision);

    auto next_target = *document.writeTarget(object);
    Eigen::Vector3d invalid = second;
    invalid.x() = std::numeric_limits<double>::quiet_NaN();
    auto rejected = document.setField<Transform>(next_target, "Transform3D.translation", "Translation", field, invalid);
    assert(!rejected && rejected.error().code == editing::EEditError::PRECONDITION_FAILED && read() == first);
    assert(document.historyView()->history.revision == committed.history.revision);
    auto retried = document.setField<Transform>(next_target, "Transform3D.translation", "Translation", field, second);
    assert(retried && read() == second);
    assert(document.undo() && read() == first);
    assert(document.undo() && read() == original);
    assert(document.redo() && read() == first);
    assert(document.undo() && read() == original);

    const auto update = [&](const scene::FieldEditToken &token, const Eigen::Vector3d &next)
    {
        if (!document.fieldEditWritable(token))
        {
            return document.fieldEdited(token);
        }
        auto *live = const_cast<Transform *>(
            static_cast<const Transform *>(document.component(object, lux::cxx::typeToken<Transform>())));
        live->translation = next;
        return document.fieldEdited(token);
    };
    const auto before_preview = *document.historyView();
    auto begun = document.beginFieldEdit<Transform, Eigen::Vector3d>(*document.writeTarget(object), "inspector-A",
                                                                     "Transform3D.translation", "Translation", field);
    assert(begun);
    assert(update(*begun, first) && read() == first);
    assert(update(*begun, second) && read() == second);
    auto wrong = *begun;
    ++wrong.sequence;
    auto stale_gesture = update(wrong, original);
    assert(!stale_gesture && stale_gesture.error().code == editing::EEditError::STALE_TARGET && read() == second);
    auto bad_preview = update(*begun, invalid);
    assert(!bad_preview && bad_preview.error().code == editing::EEditError::PRECONDITION_FAILED && read() == original);
    assert(document.historyView()->history.revision == before_preview.history.revision);
    assert(update(*begun, second));
    auto finished = document.finishFieldEdit(*begun);
    assert(finished && read() == second);
    assert(document.historyView()->history.entry_count == 1 && document.historyView()->history.cursor == 1);
    assert(document.undo() && read() == original);
    const auto before_noop = *document.historyView();
    auto noop = document.setField<Transform>(*document.writeTarget(object), "Transform3D.translation", "Translation",
                                             field, original);
    assert(noop && noop->effect == editing::EEditEffect::NO_CHANGE);
    assert(document.historyView()->history.revision == before_noop.history.revision);
    assert(document.historyView()->redo == editing::EHistoryActionAvailability::READY);

    auto cancelled = document.beginFieldEdit<Transform, Eigen::Vector3d>(
        *document.writeTarget(object), "inspector-B", "Transform3D.translation", "Translation", field);
    assert(cancelled && update(*cancelled, second));
    auto undo = document.undo();
    assert(undo && undo->outcome == editing::EHistoryTargetOutcome::CONTENT_APPLIED && read() == original);
    assert(document.historyView()->history.revision.value == before_noop.history.revision.value + 2);
    auto late = document.finishFieldEdit(*cancelled);
    assert(!late && late.error().code == editing::EEditError::STALE_TARGET);
    assert(document.redo() && read() == second);
    assert(document.undo() && read() == original);

    std::printf(
        "scene-edit: stale=%u invalid=%u old-token=%u notices=%zu revision=%llu cursor=%zu redo=1 input-preserved=1\n",
        unsigned(stale.error().code), unsigned(rejected.error().code), unsigned(late.error().code), notices,
        document.historyView()->history.revision.value, document.historyView()->history.cursor);
}
