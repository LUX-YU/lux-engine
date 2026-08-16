#pragma once
/**
 * @file ProjectManifest.hpp
 * @brief In-memory schema for `.luxproject` TOML files.
 *
 * A `.luxproject` is the entry-point manifest for a user's workspace. The
 * editor finds it first, parses it into `ProjectManifest`, then uses the
 * surrounding directory layout (`Content/`, `Worlds/`, `Source/`, `.lux/`)
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
 *   default_world  = "Worlds/Main.luxworld"            # optional, relative POSIX path
 *   binary_name    = "MyAwesomeGame"                   # optional, shipped exe name; defaults to name
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

#include <lux/engine/authoring/project/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/extension_abi/ModuleAbi.hpp>

#include <uuid.h>

#include <filesystem>
#include <string>
#include <vector>

namespace lux::authoring
{
    struct ProjectExtensionEntry final
    {
        lux::extensions::ExtensionId id;
        std::filesystem::path path;
        lux::extensions::EExtensionModuleTarget target{
            lux::extensions::EExtensionModuleTarget::RUNTIME};
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};
    };

    struct LUX_ENGINE_AUTHORING_PROJECT_PUBLIC ProjectManifest
    {
        std::string  name;                      ///< [project].name
        std::string  display_name;              ///< [project].display_name
        uuids::uuid  project_guid;              ///< [project].project_guid
        std::string  engine_version;            ///< [project].engine_version
        std::string  default_world;             ///< [project].default_world; relative POSIX

        /// [project].binary_name — what the SHIPPED executable is called
        /// (`MyGame` -> `MyGame.exe` / `libMyGame.so`). Optional; empty means
        /// "use `name`".
        ///
        /// Separate from `name` and `display_name` because all three answer
        /// different questions: `name` identifies the project on disk,
        /// `display_name` is what a human reads in a window title, and this is
        /// a FILENAME — it has to survive a filesystem, so the packaging step
        /// is the one place allowed to sanitise it.
        ///
        /// Why it lives in the manifest at all: the player double-clicks an
        /// executable, so the pack it loads is found relative to that
        /// executable's own name. Whoever writes the binary and whoever writes
        /// the pack must agree, and the manifest is where they both look.
        std::string  binary_name;

        /// Explicit module set. Paths are project-root-relative unless the
        /// author deliberately supplied an absolute deployment path.
        std::vector<ProjectExtensionEntry> extensions;

        /// The executable's base name: `binary_name` if set, else `name`.
        /// Use this rather than reading the field directly — the fallback is
        /// the whole point of the field being optional.
        [[nodiscard]] std::string binaryName() const
        {
            return binary_name.empty() ? name : binary_name;
        }

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
        /// `default_world` empty (the editor populates it after a world
        /// is saved into the project).
        [[nodiscard]] static ProjectManifest makeDefault(std::string_view project_name);
    };

} // namespace lux::authoring
