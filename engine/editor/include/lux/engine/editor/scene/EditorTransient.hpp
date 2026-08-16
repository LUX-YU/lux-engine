#pragma once
/**
 * @file EditorTransient.hpp
 * @brief Editor-scaffolding tag and Authoring filter predicate.
 *
 * This is an EDITOR concept. The Editor's Authoring transaction filters this
 * tag before invoking the per-Actor LXAD adapter; neither Authoring codecs nor
 * Runtime know what editor scaffolding means.
 */

#include <lux/engine/meta/LuxObject.hpp>

namespace lux::editor
{
    /// Tag: this entity is EDITOR SCAFFOLDING (the viewport camera, the
    /// bringUp reference grid) — session-owned, never scene CONTENT.
    /// Tagged entities are skipped by the Authoring transaction, so files and Edit/Play
    /// snapshots stay pure content (this is what fixed the "editor camera
    /// serialized into the scene and ratcheting one copy per save"
    /// accumulation), and EditorScene's play-restore PRESERVES them instead of
    /// destroy+reload (no re-find / re-bind dance). Deliberately UNREFLECTED —
    /// it must never appear in the Inspector or round-trip through any file.
    struct EditorTransientComponent {};

    /// Captureless predicate used by the Editor's Authoring transaction.
    inline bool skipEditorTransient(
        const lux::meta::EntityRegistryBase& reg,
        entt::entity e)
    {
        return reg.all_of<EditorTransientComponent>(e);
    }
} // namespace lux::editor
