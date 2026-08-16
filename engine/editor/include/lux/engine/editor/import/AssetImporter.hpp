#pragma once
/**
 * @file AssetImporter.hpp
 * @brief Editor-side orchestrator that drops an external asset file (.glb /
 *        .gltf / .fbx / .obj / .png / ...) into the project's Content/ tree.
 *
 * For each supported format the importer:
 *   1. Routes the source path to the appropriate `lux::asset::*SerDeser`.
 *   2. Materializes every produced sub-asset (mesh / material / texture /
 *      skeleton / animation clip) into the live AssetManager.
 *   3. Writes each sub-asset to a `.luxasset` file under
 *        <dest_root>/Models/<source_stem>/   (for model files)
 *        <dest_root>/Textures/                (for raw image files)
 *      so the editor can find them via the AssetBrowser the next time the
 *      project is opened.
 *   4. For model files, writes a parent `.luxmodel` manifest that records
 *      the cross-references (mesh ↔ material ↔ skeleton ↔ clip) — that's
 *      the asset the user drags into the viewport to spawn an entity.
 *
 * The importer is intentionally synchronous and main-thread-friendly: a
 * typical CesiumMan glb (~1.2 MB) imports in ~100 ms on Assimp. Larger fbx
 * files may stutter the UI; deferring to a worker thread is M3 polish.
 */

#include <lux/engine/editor/visibility.h>

#include <lux/engine/resource/asset/Asset.hpp>   // asset_id_t

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    /// Why an import succeeded / failed at a glance.
    enum class ImportResult : uint8_t
    {
        OK,                  ///< All sub-assets imported + persisted
        UnsupportedFormat,   ///< File extension not recognized
        SourceNotFound,      ///< source path does not exist on disk
        DestUnwritable,      ///< could not create the destination folder
        ImportFailed,        ///< SerDeser::importFromFile returned an error
        WriteFailed,         ///< at least one `.luxasset` could not be written
    };

    /// Summary of one `importExternalFile` call.
    struct ImportReport
    {
        /// Every file successfully written to disk, in write order. Useful
        /// for the editor's "Imported N assets" toast + AssetBrowser
        /// refresh that incremental-mounts these without a full rescan.
        std::vector<std::filesystem::path> written;

        /// The "primary" asset the user typically wants to drag into a
        /// scene next. For model imports this is the `.luxmodel`; for
        /// standalone image imports this is the TextureAsset's id. Empty
        /// when nothing was produced (UnsupportedFormat / SourceNotFound).
        std::optional<lux::asset::asset_id_t> primary_asset;

        ImportResult result  = ImportResult::OK;
        std::string  message;
    };

    /// User-facing import knobs, baked into the produced asset (model imports
    /// only; textures ignore these). Defaults are a no-op import.
    struct ImportOptions
    {
        /// Pre-rotation baked into the model, as XYZ Euler degrees. The common
        /// up-axis fix for a glTF Z-up character in this Y-up engine is
        /// (-90, 0, 0). Applied via Skeleton::global_transform for skinned
        /// meshes and into vertices for static meshes.
        float pre_rotate_x_deg{ 0.0f };
        float pre_rotate_y_deg{ 0.0f };
        float pre_rotate_z_deg{ 0.0f };
        /// Uniform scale baked alongside the rotation (e.g. 0.01 for cm→m).
        float uniform_scale{ 1.0f };
        /// Import the source's animations as AnimationClip assets.
        bool  import_animations{ true };
    };

    /// Top-level dispatcher. @p source must be absolute. @p dest_root is the
    /// project's Content/ folder; the importer creates child directories as
    /// needed. @p manager is the live AssetManager that owns every produced
    /// sub-asset for the lifetime of the editor (it is left registered in
    /// memory so subsequent `spawn*Entity` calls can dereference the ids).
    /// @p options bakes axis/scale/animation choices into model imports.
    LUX_EDITOR_PUBLIC
    ImportReport importExternalFile(
        const std::filesystem::path& source,
        const std::filesystem::path& dest_root,
        std::shared_ptr<lux::asset::AssetManager> manager,
        const ImportOptions& options = {});

    /// 批H2 池上导入核:与 importExternalFile 同一套物化+写盘,但对着一个
    /// **散管理器**(池线程构造的临时 AssetManager,账本线程锚点即池线程,
    /// 断言自然成立)—— 零活账本触碰(§8 总纪律)。产出只有盘上文件与
    /// 报告;注册进活账本由主线程 registerImportedFiles 做。
    /// ⚠️ 判重语义变化:散管理器没有活账本的确定性 id 碰撞闸 —— 调用方
    /// (ImportController)在派发前做**盘上判重**。
    LUX_EDITOR_PUBLIC
    ImportReport importExternalFileDetached(
        const std::filesystem::path& source,
        const std::filesystem::path& dest_root,
        const ImportOptions& options = {});

    /// 批H2 完成侧:把异步导入产出的文件注册进**活**管理器(账本线程调用)。
    /// 逐文件走 Shells 惰性路径(工程打开同款;MODEL/SCRIPT/SHADER 自动回落
    /// Eager)。已注册的 id(并发导入同源等)逐文件记日志跳过。返回成功数。
    LUX_EDITOR_PUBLIC
    std::size_t registerImportedFiles(
        const std::vector<std::filesystem::path>& files,
        std::shared_ptr<lux::asset::AssetManager> manager);

    /// True if @p extension is a 3D model format (so the editor knows to show
    /// the Import Options dialog before importing). Texture-only imports skip it.
    LUX_EDITOR_PUBLIC
    [[nodiscard]] bool isModelExtension(std::string_view extension);

    /// The single source of truth for importable extensions (leading dot,
    /// lowercased): the registry the dispatch + predicates derive from. The
    /// file-dialog "Import" filter derives its list from this so the dialog
    /// can never offer a format the importer then rejects (audit 7.2).
    LUX_EDITOR_PUBLIC
    [[nodiscard]] std::vector<std::string_view> importableExtensions();

    /// How `registerContentFolder` materializes each file into the manager.
    enum class ELoadMode : uint8_t
    {
        /// Fully deserialize info + data eagerly (legacy behaviour). Every
        /// asset is present-with-data on the first frame. Used by headless /
        /// tool paths and as the regression fallback.
        Eager,
        /// Register an info-only SHELL (data()==null) for every lazily-loadable
        /// type; the async path (AssetClient::request, fired by the
        /// renderable bridge / thumbnail service on first use) streams the data
        /// in on demand — this is the large-world W2b streaming path. Types that
        /// cannot be shelled (MODEL / SCRIPT / SHADER) fall back to Eager
        /// per-file so their UUIDs never dangle.
        Shells,
    };

    /// Walk @p content_root recursively and register every `.luxasset` /
    /// `.luxmodel` it finds into @p manager. Returns the number of files
    /// successfully registered. Failures (corrupt header / wrong magic /
    /// missing decoder) are logged to stderr and the file is skipped —
    /// the editor should still come up if some assets are unusable.
    ///
    /// Called from `LuxEditor::openProject` so cross-references between
    /// just-restored assets (Model → Mesh / Material / Texture / etc.)
    /// resolve via the AssetManager from the first frame.
    ///
    /// @p mode selects eager full-load vs. info-only shells (see ELoadMode).
    LUX_EDITOR_PUBLIC
    std::size_t registerContentFolder(
        const std::filesystem::path& content_root,
        std::shared_ptr<lux::asset::AssetManager> manager,
        ELoadMode mode = ELoadMode::Eager);

} // namespace lux::editor
