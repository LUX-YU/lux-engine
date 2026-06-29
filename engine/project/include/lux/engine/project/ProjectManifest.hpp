#pragma once
/**
 * @file ProjectManifest.hpp
 * @brief In-memory schema for `.luxproject` TOML files.
 *
 * A `.luxproject` is the entry-point manifest for a user's workspace. The
 * editor finds it first, parses it into `ProjectManifest`, then uses the
 * surrounding directory layout (`Content/`, `Scenes/`, `Source/`, `.lux/`)
 * relative to its location.
 *
 * Schema (M3a):
 *
 * ```toml
 *   [project]
 *   name           = "MyGame"                          # required, non-empty
 *   display_name   = "My Awesome Game"                 # optional, defaults to name
 *   project_guid   = "550e8400-e29b-41d4-a716-..."     # required, stable across saves
 *   engine_version = "0.1.0"                           # required
 *   default_scene  = "Scenes/Main.luxscene"            # optional, relative POSIX path
 *
 *   # [modules]  — future (M3b/c); module list lives here
 *   # [plugins]  — future; third-party plugin references
 *   # [settings] — future; project-wide overrides for engine defaults
 * ```
 *
 * The schema is intentionally additive: unknown TOML keys are ignored on
 * load (so a newer manifest opens cleanly in older code), and the loader
 * fills in sensible defaults for missing optional fields.
 */

#include <lux/engine/project/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <uuid.h>

#include <filesystem>
#include <string>

namespace lux::project
{
    struct LUX_ENGINE_PROJECT_PUBLIC ProjectManifest
    {
        std::string  name;                      ///< [project].name
        std::string  display_name;              ///< [project].display_name
        uuids::uuid  project_guid;              ///< [project].project_guid
        std::string  engine_version;            ///< [project].engine_version
        std::string  default_scene;             ///< [project].default_scene; relative POSIX

        /// Load + parse a `.luxproject` file. Returns a populated manifest
        /// on success; on failure an explanatory string (`"missing required
        /// field 'project.name'"`, `"toml parse error at line 5"`, etc.).
        [[nodiscard]] static lux::cxx::expected<ProjectManifest, std::string>
            loadFromFile(const std::filesystem::path& path);

        /// Write this manifest to `path` as TOML. Overwrites the file
        /// atomically (write to temp + rename) to avoid leaving a
        /// corrupted manifest on partial-write.
        [[nodiscard]] lux::cxx::expected<void, std::string>
            saveToFile(const std::filesystem::path& path) const;

        /// Build a sensible default for "New Project" flows. Generates a
        /// fresh GUID; sets engine_version to the current build's version;
        /// `default_scene` empty (the editor populates it after a scene
        /// is saved into the project).
        [[nodiscard]] static ProjectManifest makeDefault(std::string_view project_name);
    };

} // namespace lux::project
