#pragma once
/**
 * @file DemoSceneTemplate.hpp
 * @brief One-shot "create a demo scene file on disk" helper.
 *
 * Drives a brand-new `.luxworld` that contains the engine's demo content
 * (Skybox + Grid + Sun + Floor + Cube + Editor Camera) so a user can
 * pick "New Demo Project" in the launcher and immediately see something
 * recognisable in the editor.
 *
 * Pure I/O — takes a destination path, builds an EntityRegistry in memory,
 * writes one external Actor document per entity, then atomically commits the
 * LXWA root last. Does NOT bring up render resources or interact with
 * `EditorScene`. Intended for the launcher's project-template flow.
 *
 * Why the editor library (not the launcher): this code needs the
 * gameplay component types, the injected `ComponentTypeCatalog`, and the
 * LXAD Actor adapter, and the built-in asset UUIDs. The
 * editor static library already aggregates all of them; making this
 * a helper here lets the launcher link the editor lib to get the
 * one entry point without duplicating any of that surface.
 *
 * The matching asset UUIDs referenced by the demo scene are the same
 * ones `EditorBuiltins` registers into AssetManager at editor
 * startup, so loading the demo scene in the editor process resolves
 * cleanly without further setup.
 */

#include <lux/engine/editor/visibility.h>

#include <filesystem>

namespace lux::ecs { class ComponentTypeCatalog; }

namespace lux::editor
{
    /// Write a fresh demo `.luxworld` to @p scene_file_path. Creates the
    /// parent directory if needed. Returns true on success; on failure
    /// writes a diagnostic to stderr and returns false. The caller
    /// (typically the launcher) is responsible for ensuring meta-module
    /// init has run so the reflection / component type registries are
    /// populated — without it, external Actor documents contain zero
    /// reflected components.
    LUX_EDITOR_PUBLIC
    bool writeDemoScene(
        const std::filesystem::path& scene_file_path,
        const lux::ecs::ComponentTypeCatalog& components);

} // namespace lux::editor
