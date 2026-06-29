#pragma once
/**
 * @file DemoSceneTemplate.hpp
 * @brief One-shot "create a demo scene file on disk" helper.
 *
 * Drives a brand-new `.luxscene` that contains the engine's demo content
 * (Skybox + Grid + Sun + Floor + Cube + Editor Camera) so a user can
 * pick "New Demo Project" in the launcher and immediately see something
 * recognisable in the editor.
 *
 * Pure I/O — takes a destination path, builds an entt::registry in
 * memory, runs `lux::editor::Scene::save`. Does NOT bring up any
 * render resources or interact with `EditorScene`. Intended for the
 * launcher's project-template flow.
 *
 * Why the editor library (not the launcher): this code needs the
 * gameplay component types, `ComponentTypeRegistry`, the
 * `lux::editor::Scene` writer, and the built-in asset UUIDs. The
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

namespace lux::editor
{
    /// Write a fresh demo `.luxscene` to @p scene_file_path. Creates the
    /// parent directory if needed. Returns true on success; on failure
    /// writes a diagnostic to stderr and returns false. The caller
    /// (typically the launcher) is responsible for ensuring meta-module
    /// init has run so the reflection / component type registries are
    /// populated — without it, Scene::save writes a scene with zero
    /// components.
    LUX_EDITOR_PUBLIC
    bool writeDemoScene(const std::filesystem::path& scene_file_path);

    /// Write a LARGE, complex demo `.luxscene` to @p scene_file_path: a
    /// city-scale grid of buildings + spheres spread across thousands of world
    /// units (so the large-world streaming kicks in), a directional sun, and
    /// many coloured point + spot lights — EACH paired with a co-located
    /// emissive "bulb" mesh so the light SOURCE is visible, not just its effect.
    /// References only EditorBuiltins assets (primitives + emissive palette), so
    /// it resolves in the editor without extra setup. Same meta-init caveat as
    /// writeDemoScene.
    LUX_EDITOR_PUBLIC
    bool writeBigDemoScene(const std::filesystem::path& scene_file_path);

} // namespace lux::editor
