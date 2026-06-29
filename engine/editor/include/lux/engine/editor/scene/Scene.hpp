#pragma once
/**
 * @file Scene.hpp
 * @brief `.luxscene` file I/O — the dev-time persistence format for the
 *        editor's World.
 *
 * Scenes are NOT assets. They don't go through `AssetManager`, don't have
 * a UUID identity in the asset registry, and don't get treated as objects
 * by the cooker (M3b's cooker reads `.luxscene` files directly and bakes
 * their entity tree into the runtime `.luxpackage`). The `Scene` class is
 * a stateless file-I/O facade: `save` walks an `entt::registry` and
 * writes a binary file; `load` reads a file back into a registry.
 *
 * Binary format:
 *
 * ```
 *   SceneFileHeader (LXSC magic + version + scene_guid + section offsets/sizes)
 *   NameTable                       // dedup'd strings used by both sections
 *   u32 entity_count
 *   repeat entity_count times:
 *     u64 serialized_entity_handle  // opaque; only used to resolve intra-scene refs
 *     u32 component_count
 *     repeat component_count times:
 *       u32 component_type_name_idx
 *       u32 component_payload_size  // skip if type unknown
 *       <TaggedPropertyWriter::writeObject for this component>
 *   u32 active_camera_entity_index  // index into the entity array, or 0xFFFFFFFF
 * ```
 *
 * Forward compat: unknown component types and unknown fields inside known
 * components are skipped by their declared size — older code loads a newer
 * scene file without error.
 */

#include <lux/engine/editor/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <entt/entt.hpp>

#include <uuid.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lux::meta
{
    class EntityRegistry;
} // namespace lux::meta

namespace lux::editor
{
    /// Header laid down at byte 0 of every `.luxscene` file.
    struct SceneFileHeader
    {
        std::uint32_t magic;            ///< Must be `kSceneMagic` ("LXSC").
        std::uint32_t format_version;   ///< Bumped on incompatible layout changes.
        uuids::uuid   scene_guid;       ///< Stable across saves of the same scene.
        std::uint64_t name_table_offset;
        std::uint64_t name_table_size;
        std::uint64_t entity_data_offset;
        std::uint64_t entity_data_size;
    };

    /// "LXSC" little-endian.
    inline constexpr std::uint32_t kSceneMagic         = 0x4353584Cu;
    inline constexpr std::uint32_t kSceneFormatVersion = 1u;

    struct SceneSaveOptions
    {
        /// Optional active-camera entity. If valid + present in the
        /// registry, it's recorded so `Scene::load` can return it.
        entt::entity active_camera{entt::null};

        /// Optional scene GUID. Use the empty UUID to ask Scene::save
        /// to mint a fresh one on disk.
        uuids::uuid  scene_guid{};
    };

    struct SceneLoadResult
    {
        std::vector<entt::entity> created_entities; ///< In file order.
        entt::entity              active_camera{entt::null};
        uuids::uuid               scene_guid{};
    };

    /// Static facade for `.luxscene` file I/O. Pure file ops — no
    /// registry ownership / no AssetManager.
    class LUX_EDITOR_PUBLIC Scene
    {
    public:
        Scene() = delete;

        /// Serialise every entity in @p registry to @p path.
        ///
        /// Walks `ComponentTypeRegistry::instance().all()` for each entity
        /// to enumerate which components are present; serialises each via
        /// reflection (`TaggedPropertyArchive`). Atomic write via
        /// temp-file + rename — a kill during save will not corrupt the
        /// existing file.
        ///
        /// Errors: filesystem failure, parent directory missing & can't
        /// create, write failure.
        [[nodiscard]] static lux::cxx::expected<void, std::string>
            save(const std::filesystem::path& path,
                 lux::meta::EntityRegistry&   registry,
                 const SceneSaveOptions&      opts = {});

        /// Deserialise the file at @p path into @p registry.
        ///
        /// Creates fresh entities for everything the file describes; does
        /// NOT clear pre-existing entities (caller is responsible for
        /// load-into-empty vs additive-merge semantics).
        ///
        /// Errors: file missing, bad magic / version, truncated body,
        /// schema corruption.
        [[nodiscard]] static lux::cxx::expected<void, std::string>
            load(const std::filesystem::path& path,
                 lux::meta::EntityRegistry&   registry,
                 SceneLoadResult&             out);
    };

} // namespace lux::editor
