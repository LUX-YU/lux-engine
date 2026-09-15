#pragma once
#include <cassert>
#include <cstdio>
#include <limits>
#include <lux/engine/editor/gui/asset/AssetReference.hpp>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

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
        std::ranges::find_if(document.objects(),
                             [&](const auto &item)
                             {
                                 return document.component(item.object, lux::cxx::typeToken<Transform>()) != nullptr;
                             });
    assert(row != document.objects().end());
    const auto object = row->object;
    const auto read = [&]() -> Eigen::Vector3d
    {
        return static_cast<const Transform *>(document.component(object, lux::cxx::typeToken<Transform>()))
            ->translation;
    };
    const auto field = [](auto &value) noexcept
    {
        return &value.translation;
    };
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

    const auto before_preview = *document.historyView();
    auto begun = document.beginPreview<Transform, Eigen::Vector3d>(*document.writeTarget(object), "inspector-A",
                                                                   "Transform3D.translation", "Translation", field);
    assert(begun);
    assert(document.updatePreview(*begun, first) && read() == first);
    assert(document.updatePreview(*begun, second) && read() == second);
    auto wrong = *begun;
    ++wrong.sequence;
    auto stale_gesture = document.updatePreview(wrong, original);
    assert(!stale_gesture && stale_gesture.error().code == editing::EEditError::STALE_TARGET && read() == second);
    auto bad_preview = document.updatePreview(*begun, invalid);
    assert(!bad_preview && bad_preview.error().code == editing::EEditError::PRECONDITION_FAILED && read() == second);
    assert(document.historyView()->history.revision == before_preview.history.revision);
    auto finished = document.commitPreview(*begun);
    assert(finished && read() == second);
    assert(document.historyView()->history.entry_count == 1 && document.historyView()->history.cursor == 1);
    assert(document.undo() && read() == original);
    const auto before_noop = *document.historyView();
    auto noop = document.setField<Transform>(*document.writeTarget(object), "Transform3D.translation", "Translation",
                                             field, original);
    assert(noop && noop->effect == editing::EEditEffect::NO_CHANGE);
    assert(document.historyView()->history.revision == before_noop.history.revision);
    assert(document.historyView()->redo == editing::EHistoryActionAvailability::READY);

    auto cancelled = document.beginPreview<Transform, Eigen::Vector3d>(*document.writeTarget(object), "inspector-B",
                                                                       "Transform3D.translation", "Translation", field);
    assert(cancelled && document.updatePreview(*cancelled, second));
    auto undo = document.undo();
    assert(undo && undo->outcome == editing::EHistoryTargetOutcome::TRANSIENT_CANCELLED && read() == original);
    assert(document.historyView()->history.revision == before_noop.history.revision);
    auto late = document.commitPreview(*cancelled);
    assert(!late && late.error().code == editing::EEditError::STALE_TARGET);
    assert(document.redo() && read() == second);
    assert(document.undo() && read() == original);

    std::printf(
        "scene-edit: stale=%u invalid=%u old-token=%u notices=%zu revision=%llu cursor=%zu redo=1 input-preserved=1\n",
        unsigned(stale.error().code), unsigned(rejected.error().code), unsigned(late.error().code), notices,
        document.historyView()->history.revision.value, document.historyView()->history.cursor);
}
