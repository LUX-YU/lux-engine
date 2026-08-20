#include <lux/engine/editor/import/AssetImporter.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/codecs/AnimationClipSerDeser.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>   // readAssetHeader / assetTypeOfMagic
#include <lux/engine/resource/asset/codecs/MaterialSerDeser.hpp>
#include <lux/engine/resource/asset/MaterialInstanceAsset.hpp>
#include <lux/engine/resource/asset/codecs/MaterialInstanceSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/ScriptSerDeser.hpp>     // .lua -> SCRIPT asset
#include <lux/engine/resource/asset/MeshAsset.hpp>
#include <lux/engine/resource/asset/codecs/MeshSerDeser.hpp>
#include <lux/engine/resource/asset/ModelAsset.hpp>
#include <lux/engine/toolchain/asset/model/ModelImporter.hpp>
#include <lux/engine/resource/asset/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/codecs/SkeletonSerDeser.hpp>
#include <lux/engine/resource/asset/TextureAsset.hpp>
#include <lux/engine/resource/asset/codecs/TextureCodec.hpp>
#include <lux/engine/authoring/assets/FlowGraphSerDeser.hpp>
#include <lux/engine/toolchain/asset/texture/TextureImporter.hpp>

// import->graph (#50): editor/cook-tier auto-conversion of imported materials to
// baked graph materials (always built with the editor — shadergen_glsl is forced on).
#include <lux/engine/toolchain/asset/material/MaterialGraphCompiler.hpp>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/toolchain/asset/material/MaterialToGraph.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <lux/cxx/core/Format.hpp>
#include <fstream>
#include <numbers>
#include <string_view>
#include <system_error>

#include <Eigen/Geometry>

namespace lux::editor
{
    namespace
    {
        // Lower-case ASCII copy — std::tolower has UB on signed-char inputs.
        std::string toLowerAscii(std::string_view s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                [](char c) { return static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))); });
            return out;
        }

        // ── importer registry: the SINGLE source of truth for "which
        // extensions are importable and how" (editor ADR §7.3 audit 7.1/7.2).
        // Adding a source family (audio, font) = one row here; the dispatch,
        // the two public predicates, AND the file-dialog filters all derive
        // from it, so they can never drift apart again.
        enum class EImporterKind : std::uint8_t { Model, Texture, Script };
        struct ImporterEntry { std::string_view ext; EImporterKind kind; };
        constexpr ImporterEntry kImporters[] = {
            {".glb", EImporterKind::Model},  {".gltf", EImporterKind::Model},
            {".fbx", EImporterKind::Model},  {".obj",  EImporterKind::Model},
            {".png", EImporterKind::Texture},{".jpg",  EImporterKind::Texture},
            {".jpeg",EImporterKind::Texture},{".tga",  EImporterKind::Texture},
            {".bmp", EImporterKind::Texture},{".dds",  EImporterKind::Texture},
            {".ktx2",EImporterKind::Texture},{".hdr",  EImporterKind::Texture},
            // Lua source scripts become SCRIPT assets (LuaSourceScript).
            {".lua", EImporterKind::Script},
        };
        [[nodiscard]] const ImporterEntry* importerFor(std::string_view ext) noexcept
        {
            for (const auto& e : kImporters) if (e.ext == ext) return &e;
            return nullptr;
        }
        bool isModelExt(std::string_view ext) noexcept
        {
            const auto* e = importerFor(ext);
            return e && e->kind == EImporterKind::Model;
        }
        bool isTextureExt(std::string_view ext) noexcept
        {
            const auto* e = importerFor(ext);
            return e && e->kind == EImporterKind::Texture;
        }

        // Make a sanitized filename out of an arbitrary asset display
        // intent (e.g. an aiAnimation name). For now we just allow ASCII
        // word chars + '-'; everything else becomes '_'.
        std::string sanitizeFilename(std::string_view in, std::string_view fallback)
        {
            std::string out;
            out.reserve(in.size());
            for (char c : in)
            {
                if (std::isalnum(static_cast<unsigned char>(c)) ||
                    c == '-' || c == '_')
                    out.push_back(c);
                else
                    out.push_back('_');
            }
            if (out.empty()) out.assign(fallback);
            return out;
        }

        // Raw mtime ticks of a source file (0 on error). Used only for change
        // detection (re-import), so the file-clock's epoch base doesn't matter
        // as long as it is stable across runs for an unchanged file.
        std::uint64_t sourceMtimeTicks(const std::filesystem::path& source)
        {
            std::error_code ec;
            const auto t = std::filesystem::last_write_time(source, ec);
            return ec ? 0u
                      : static_cast<std::uint64_t>(t.time_since_epoch().count());
        }

        // Stamp asset format v2 provenance onto an already-registered asset
        // before it is persisted, so the written .luxasset is self-describing
        // (human name) and re-importable (where it came from + when). The
        // strings are truncated to the fixed AssetInfo capacities.
        void stampInfo(lux::asset::AssetManager&     manager,
                       const lux::asset::asset_id_t& id,
                       std::string_view              display_name,
                       std::string_view              source_path,
                       std::uint64_t                 source_mtime)
        {
            auto* asset = manager.fetchAsset(id);
            if (!asset) return;
            auto* info = asset->mutableInfo();
            if (!info) return;

            const auto setField = [](char* dst, std::size_t cap, std::string_view s)
            {
                const std::size_t n = std::min(s.size(), cap - 1);
                if (n) std::memcpy(dst, s.data(), n);
                dst[n] = '\0';
            };
            setField(info->display_name, sizeof(info->display_name), display_name);
            setField(info->source_path,  sizeof(info->source_path),  source_path);
            info->source_mtime = source_mtime;
        }

        // Helper: stamp v2 provenance, then write one sub-asset to disk through
        // its dedicated SerDeser. The display name defaults to the destination
        // file stem. Returns true on success and appends to `report.written`;
        // on failure sets `report.result = WriteFailed` and pushes a message.
        template <class SerDeser>
        bool writeSub(SerDeser&                          ser,
                      lux::asset::AssetManager&          manager,
                      const lux::asset::asset_id_t&      id,
                      const std::filesystem::path&       file,
                      const char*                        category_label,
                      std::string_view                   source_path,
                      std::uint64_t                      source_mtime,
                      ImportReport&                      report)
        {
            stampInfo(manager, id, file.stem().string(), source_path, source_mtime);
            const auto err = ser.exportAsLuxAsset(id, file);
            if (err != lux::asset::EAssetError::SUCCESS)
            {
                report.result  = ImportResult::WriteFailed;
                report.message = lux::format(
                    "[AssetImporter] failed to write {} '{}' (err={})",
                    category_label, file.string(), static_cast<int>(err));
                std::fprintf(stderr, "%s\n", report.message.c_str());
                return false;
            }
            report.written.push_back(file);
            return true;
        }

        ImportReport importModelFile(
            const std::filesystem::path& source,
            const std::filesystem::path& dest_root,
            std::shared_ptr<lux::asset::AssetManager> manager,
            const ImportOptions& options)
        {
            ImportReport report;

            // Destination folder named after the source stem under Models/.
            // Re-import now reuses the same UUIDs (deterministic ids keyed on
            // the source path, below), so the reference graph is preserved.
            // NOT yet handled: replacing the in-memory data of an already-
            // registered asset — registerAsset() refuses an existing id, so
            // re-importing an *edited* source keeps the old content until full
            // re-import lands (tech-debt §1.9). First import and distinct
            // sources both behave correctly.
            const auto folder = dest_root / "Models" / source.stem();
            std::error_code mkdir_ec;
            std::filesystem::create_directories(folder, mkdir_ec);
            if (mkdir_ec)
            {
                report.result  = ImportResult::DestUnwritable;
                report.message = lux::format(
                    "[AssetImporter] mkdir failed: {} ({})",
                    folder.string(), mkdir_ec.message());
                return report;
            }

            // ── Phase 1: Assimp → in-memory ModelAsset + sub-assets ──────
            // ModelImporter owns its sub-asset codecs; importFromFile
            // registers every produced asset into `manager` and returns the
            // top-level ModelAsset.
            lux::toolchain::ModelImporter model_importer(manager);
            // Deterministic ids: derive every sub-asset id from the canonical
            // SOURCE path (tech-debt §2.1). Re-importing the same source =>
            // same canonical path => same ids, so the on-disk reference graph
            // survives instead of being orphaned. The source path (not the
            // destination stem) is the identity: two different sources that
            // happen to share a stem — a/Hero.glb vs b/Hero.fbx — must NOT mint
            // identical ids, which a stem-based seed would do.
            //   Trade-off: keys on absolute path, so ids are stable per machine
            //   but not reproducible across machines/checkouts. Cross-machine
            //   reproducibility would need source-content hashing (deferred).
            {
                std::error_code can_ec;
                auto canon = std::filesystem::weakly_canonical(source, can_ec);
                if (can_ec || canon.empty())
                    canon = source.lexically_normal();
                model_importer.config().deterministic_seed =
                    canon.generic_string();
            }

            // Map Import Options → bake config. Euler degrees → quaternion
            // (intrinsic Z·Y·X); only the rotation+scale+animation knobs feed
            // the SerDeser, which bakes them into the produced assets.
            {
                constexpr float kDeg2Rad =
                    static_cast<float>(std::numbers::pi) / 180.0f;
                const Eigen::Quaternionf q =
                    Eigen::AngleAxisf(options.pre_rotate_z_deg * kDeg2Rad, Eigen::Vector3f::UnitZ()) *
                    Eigen::AngleAxisf(options.pre_rotate_y_deg * kDeg2Rad, Eigen::Vector3f::UnitY()) *
                    Eigen::AngleAxisf(options.pre_rotate_x_deg * kDeg2Rad, Eigen::Vector3f::UnitX());
                model_importer.config().pre_rotate        = q;
                model_importer.config().uniform_scale     =
                    (options.uniform_scale > 0.0f) ? options.uniform_scale : 1.0f;
                model_importer.config().import_animations =
                    options.import_animations;
            }
            auto imported = model_importer.importFromFile(source);
            if (!imported)
            {
                report.result  = ImportResult::ImportFailed;
                // Deterministic ids mean re-importing the same source collides
                // with the still-registered first import (registerAsset refuses
                // an existing id). Report that clearly instead of a cryptic
                // numeric code. True re-import (replace CPU data + evict the
                // render bridge's GPU upload cache) is tech-debt §1.9.
                if (imported.error() == lux::asset::EAssetError::ASSET_ALREADY_EXIST)
                    report.message = lux::format(
                        "[AssetImporter] '{}' is already imported — re-import of "
                        "edited sources is not yet supported (§1.9).",
                        source.filename().string());
                else
                    report.message = lux::format(
                        "[AssetImporter] importFromFile failed (err={})",
                        static_cast<int>(imported.error()));
                std::fprintf(stderr, "%s\n", report.message.c_str());
                return report;
            }
            const auto* model = static_cast<const lux::asset::ModelAsset*>(
                imported.value().first);
            const auto  model_id = imported.value().second;

            // ── Phase 2: persist every sub-asset under `folder` ──────────
            //
            // Write order is texture → material → mesh → skeleton → clip →
            // .luxmodel. The order does not actually matter for correctness
            // (cross-references are by uuid, persisted in each `.luxasset`'s
            // own info section), but writing the manifest LAST means a
            // crash mid-import leaves a manifest-free directory that
            // re-import can safely overwrite without orphan manifests.
            lux::asset::TextureCodec          tex_ser(manager);
            lux::asset::MeshSerDeser          mesh_ser(manager);
            lux::asset::SkeletonSerDeser      skel_ser(manager);
            lux::asset::AnimationClipSerDeser clip_ser(manager);

            // Provenance shared by every sub-asset of this import.
            const std::string  src_str = source.string();
            const std::uint64_t src_mtime = sourceMtimeTicks(source);

            // Gather every TextureAsset id referenced by any imported-material desc
            // ModelImporter carries per-desc texture UUID lists on the model,
            // keyed by desc ordinal; there are no closure MaterialAssets anymore).
            std::vector<lux::asset::asset_id_t> texture_ids;
            for (const auto& uuids : model->importedMaterialTextureUuids())
                for (const auto& tid : uuids)
                    if (std::find(texture_ids.begin(), texture_ids.end(), tid)
                        == texture_ids.end())
                        texture_ids.push_back(tid);

            for (size_t i = 0; i < texture_ids.size(); ++i)
            {
                const auto p = folder /
                    lux::format("Texture_{}.luxasset", i);
                if (!writeSub(tex_ser, *manager, texture_ids[i], p, "texture",
                              src_str, src_mtime, report))
                    return report;
            }
            // (No Material_i.luxasset — W5b retired the closure MaterialAsset; the
            //  imported materials are baked as MaterialAssets in Phase 2.5 below.)
            for (size_t i = 0; i < model->meshAssetIds().size(); ++i)
            {
                const auto p = folder /
                    lux::format("Mesh_{}.luxasset", i);
                if (!writeSub(mesh_ser, *manager, model->meshAssetIds()[i],
                              p, "mesh", src_str, src_mtime, report))
                    return report;
            }
            if (model->skeletonAssetId().has_value())
            {
                const auto p = folder / "Skeleton.luxasset";
                if (!writeSub(skel_ser, *manager, *model->skeletonAssetId(),
                              p, "skeleton", src_str, src_mtime, report))
                    return report;
            }
            for (size_t i = 0; i < model->animationClipAssetIds().size(); ++i)
            {
                const auto p = folder /
                    lux::format("Anim_{}.luxasset", i);
                if (!writeSub(clip_ser, *manager, model->animationClipAssetIds()[i],
                              p, "animation clip", src_str, src_mtime, report))
                    return report;
            }

            // ── Phase 2.5: import->graph (W5b) ───────────────────────────
            // ModelImporter emits ImportedMaterialDesc PODs.
            // Convert each to a graph + bake a GRAPH material, then FILL the model's
            // (empty) material-uuid vector positionally (== ModelMeshInfo::material_index)
            // so the Phase-3 manifest records MATERIAL uuids and the unified
            // runtime path renders them. On per-material failure we append a NIL id to
            // keep the ordinal alignment with material_index.
            if (auto* model_mut =
                    manager->fetchAssetAs<lux::asset::ModelAsset>(model_id))
            {
                std::string canon_seed;
                {
                    std::error_code can_ec;
                    auto canon = std::filesystem::weakly_canonical(source, can_ec);
                    if (can_ec || canon.empty()) canon = source.lexically_normal();
                    canon_seed = canon.generic_string();
                }
                const auto& descs    = model_mut->importedMaterialDescs();
                const auto& desc_tex  = model_mut->importedMaterialTextureUuids();
                for (std::size_t i = 0; i < descs.size(); ++i)
                {
                    auto graph_exp = lux::shadergen::material::materialToGraph(descs[i]);
                    if (!graph_exp)
                    {
                        std::fprintf(stderr,
                            "[AssetImporter] material %zu -> graph conversion skipped: %s\n",
                            i, graph_exp.error().c_str());
                        model_mut->addMaterialAssetId(lux::asset::asset_id_t{}); // nil: keep ordinal
                        continue;
                    }
                    lux::rdesc::MaterialGraph& graph = *graph_exp;
                    // graph texture slot s == ImportedTextureRef::texture_index == desc_tex[i][s].
                    std::vector<lux::asset::asset_id_t> slot_ids = desc_tex[i];

                    const auto gp = folder / lux::format("GraphMaterial_{}.luxasset", i);
                    const std::string gname =
                        lux::format("{}_GraphMat_{}", source.stem().string(), i);
                    const std::string seed = canon_seed + "|graphmat|" + std::to_string(i);

                    auto graph_id = lux::toolchain::bakeGraphMaterial(
                        manager,
                        graph,
                        slot_ids,
                        gname,
                        gp,
                        seed);
                    if (!graph_id)
                    {
                        std::fprintf(stderr,
                            "[AssetImporter] material %zu -> graph bake failed: %s\n",
                            i, graph_id.error().c_str());
                        model_mut->addMaterialAssetId(lux::asset::asset_id_t{}); // nil: keep ordinal
                        continue;
                    }
                    model_mut->addMaterialAssetId(*graph_id);  // append in desc order
                    report.written.push_back(gp);
                }
            }

            // ── Phase 3: the `.luxmodel` manifest goes last ─────────────
            const auto model_path = folder /
                (source.stem().string() + ".luxmodel");
            stampInfo(*manager, model_id, source.stem().string(),
                      src_str, src_mtime);
            const auto manifest_err = model_importer.exportAsLuxAsset(
                model_id, model_path);
            if (manifest_err != lux::asset::EAssetError::SUCCESS)
            {
                report.result  = ImportResult::WriteFailed;
                report.message = lux::format(
                    "[AssetImporter] failed to write .luxmodel '{}' (err={})",
                    model_path.string(),
                    static_cast<int>(manifest_err));
                std::fprintf(stderr, "%s\n", report.message.c_str());
                return report;
            }
            report.written.push_back(model_path);
            report.primary_asset = model_id;
            report.result        = ImportResult::OK;
            report.message       = lux::format(
                "[AssetImporter] '{}' -> {} files under '{}' "
                "(meshes={}, materials={}, textures={}, skeleton={}, clips={})",
                source.filename().string(),
                report.written.size(), folder.string(),
                model->meshAssetIds().size(),
                model->materialAssetIds().size(),
                texture_ids.size(),
                model->skeletonAssetId().has_value() ? 1 : 0,
                model->animationClipAssetIds().size());
            std::fprintf(stderr, "%s\n", report.message.c_str());
            return report;
        }

        ImportReport importTextureFile(
            const std::filesystem::path& source,
            const std::filesystem::path& dest_root,
            std::shared_ptr<lux::asset::AssetManager> manager)
        {
            ImportReport report;

            // Textures land flat under Textures/ — keep their original
            // basename for easier human lookup.
            const auto folder = dest_root / "Textures";
            std::error_code mkdir_ec;
            std::filesystem::create_directories(folder, mkdir_ec);
            if (mkdir_ec)
            {
                report.result  = ImportResult::DestUnwritable;
                report.message = lux::format(
                    "[AssetImporter] mkdir failed: {} ({})",
                    folder.string(), mkdir_ec.message());
                return report;
            }

            lux::toolchain::TextureImporter ser(manager);
            // Deterministic id keyed on the canonical SOURCE path (tech-debt
            // §2.1), NOT the sanitized stem: distinct sources whose stems
            // sanitize to the same string (wood.png vs "wood ?.png") must not
            // collide. The sanitized stem is still used for the on-disk
            // filename only.
            const auto stem = sanitizeFilename(source.stem().string(), "Texture");
            std::error_code can_ec;
            auto canon = std::filesystem::weakly_canonical(source, can_ec);
            if (can_ec || canon.empty())
                canon = source.lexically_normal();
            ser.config().deterministic_seed =
                lux::format("{}|texture", canon.generic_string());
            auto imported = ser.importFromFile(source);
            if (!imported)
            {
                report.result  = ImportResult::ImportFailed;
                report.message = lux::format(
                    "[AssetImporter] texture importFromFile failed (err={})",
                    static_cast<int>(imported.error()));
                std::fprintf(stderr, "%s\n", report.message.c_str());
                return report;
            }
            const auto tex_id = imported.value().second;
            const auto p = folder / (stem + ".luxasset");
            if (!writeSub(ser, *manager, tex_id, p, "texture",
                          source.string(), sourceMtimeTicks(source), report))
                return report;
            report.primary_asset = tex_id;
            report.message       = lux::format(
                "[AssetImporter] '{}' -> '{}'",
                source.filename().string(), p.string());
            std::fprintf(stderr, "%s\n", report.message.c_str());
            return report;
        }

        // .lua -> SCRIPT asset. Mirrors importTextureFile: import
        // through the SerDeser (deterministic id keyed on the canonical source
        // path, so RE-IMPORT keeps the id — that is the hot-reload identity),
        // then persist the .luxasset under Scripts/.
        ImportReport importScriptFile(
            const std::filesystem::path& source,
            const std::filesystem::path& dest_root,
            std::shared_ptr<lux::asset::AssetManager> manager)
        {
            ImportReport report;

            const auto folder = dest_root / "Scripts";
            std::error_code mkdir_ec;
            std::filesystem::create_directories(folder, mkdir_ec);
            if (mkdir_ec)
            {
                report.result  = ImportResult::DestUnwritable;
                report.message = lux::format(
                    "[AssetImporter] mkdir failed: {} ({})",
                    folder.string(), mkdir_ec.message());
                return report;
            }

            lux::asset::ScriptSerDeser ser(manager);
            const auto stem = sanitizeFilename(source.stem().string(), "Script");
            std::error_code can_ec;
            auto canon = std::filesystem::weakly_canonical(source, can_ec);
            if (can_ec || canon.empty())
                canon = source.lexically_normal();
            ser.config().deterministic_seed =
                lux::format("{}|script", canon.generic_string());
            auto imported = ser.importFromFile(source);
            if (!imported)
            {
                report.result  = ImportResult::ImportFailed;
                report.message = lux::format(
                    "[AssetImporter] script importFromFile failed (err={})",
                    static_cast<int>(imported.error()));
                std::fprintf(stderr, "%s\n", report.message.c_str());
                return report;
            }
            const auto script_id = imported.value().second;
            const auto p = folder / (stem + ".luxasset");
            if (!writeSub(ser, *manager, script_id, p, "script",
                          source.string(), sourceMtimeTicks(source), report))
                return report;
            report.primary_asset = script_id;
            report.message       = lux::format(
                "[AssetImporter] '{}' -> '{}'",
                source.filename().string(), p.string());
            std::fprintf(stderr, "%s\n", report.message.c_str());
            return report;
        }
    } // namespace

    bool isModelExtension(std::string_view extension)
    {
        return isModelExt(toLowerAscii(extension));
    }

    std::vector<std::string_view> importableExtensions()
    {
        std::vector<std::string_view> out;
        out.reserve(std::size(kImporters));
        for (const auto& e : kImporters) out.push_back(e.ext);
        return out;
    }

    ImportReport importExternalFile(
        const std::filesystem::path& source,
        const std::filesystem::path& dest_root,
        std::shared_ptr<lux::asset::AssetManager> manager,
        const ImportOptions& options)
    {
        ImportReport report;

        if (!std::filesystem::exists(source))
        {
            report.result  = ImportResult::SourceNotFound;
            report.message = lux::format(
                "[AssetImporter] source not found: '{}'", source.string());
            return report;
        }
        if (!manager)
        {
            report.result  = ImportResult::ImportFailed;
            report.message = "[AssetImporter] null AssetManager";
            return report;
        }

        const auto ext = toLowerAscii(source.extension().string());
        if (const auto* e = importerFor(ext))
        {
            switch (e->kind)
            {
            case EImporterKind::Model:
                return importModelFile(source, dest_root, std::move(manager), options);
            case EImporterKind::Texture:
                return importTextureFile(source, dest_root, std::move(manager));
            case EImporterKind::Script:
                return importScriptFile(source, dest_root, std::move(manager));
            }
        }

        report.result  = ImportResult::UnsupportedFormat;
        report.message = lux::format(
            "[AssetImporter] unsupported extension '{}' for '{}'",
            ext, source.string());
        return report;
    }

    // -------------------------------------------------------------------------
    //  registerContentFolder
    // -------------------------------------------------------------------------
    //
    // The first time a project opens, the editor's AssetManager is empty
    // and any World-document references inside it would dangle. This walker
    // mounts every persisted asset on disk so subsequent UUID lookups
    // (Inspector field reads, model -> mesh dereferences, ...) succeed.
    //
    // Dispatch order intentionally does NOT need to be dependency-aware:
    // each `fromLuxAsset` only deserializes that file's own info section,
    // never chases pointers to other files. Cross-references are by uuid
    // and resolved on demand (re-queried from the manager) at use time.

    namespace
    {
        // Dispatch one `.luxasset` (or `.luxmodel`) to its matching SerDeser
        // by inspecting the file header magic — the .luxasset extension alone
        // is ambiguous (one extension covers every asset type). Both the
        // magic->type is a Runtime format primitive; the manager's product
        // codec factory adds authored formats without coupling Resource to
        // Editor/Toolchain. Returns true when the file
        // registered into the manager; false on any decode failure (logged).
        // Eager full-load of one file: deserialize info + data and self-register.
        bool registerOneFileEager(const std::filesystem::path& file,
                                  std::shared_ptr<lux::asset::AssetManager> mgr_shared)
        {
            const auto probe = lux::asset::readAssetHeader(file);
            if (probe.magic == 0)
            {
                std::fprintf(stderr,
                    "[AssetImporter] skip '%s': cannot read header\n",
                    file.string().c_str());
                return false;
            }

            const auto type = lux::asset::assetTypeOfMagic(probe.magic);
            auto serdeser = mgr_shared->createSerDeser(type, mgr_shared);
            if (!serdeser)
            {
                std::fprintf(stderr,
                    "[AssetImporter] skip '%s': unknown magic 0x%08x\n",
                    file.string().c_str(), probe.magic);
                return false;
            }

            // The SerDeser self-registers the produced asset into the manager
            // (fromLuxAsset -> typed override + AssetManager::registerAsset),
            // so the returned pointer is deliberately discarded.
            auto r = serdeser->fromLuxAsset(file);
            const auto ec =
                r.has_value() ? lux::asset::EAssetError::SUCCESS : r.error();

            if (ec != lux::asset::EAssetError::SUCCESS)
            {
                // ASSET_ALREADY_EXIST is fine — re-opening a project after
                // a hot rescan can hit a file we already registered. Treat
                // as success.
                if (ec == lux::asset::EAssetError::ASSET_ALREADY_EXIST)
                    return true;
                std::fprintf(stderr,
                    "[AssetImporter] skip '%s': fromLuxAsset failed (err=%d)\n",
                    file.string().c_str(), static_cast<int>(ec));
                return false;
            }
            return true;
        }

        // Shell registration of one file: register an info-only shell so the
        // async path streams its data in on demand. Non-lazy types (MODEL /
        // SCRIPT / SHADER → makeShellFromFile returns UNSUPPORTED) fall back
        // to eager so their UUIDs are never left dangling. Other header errors
        // are reported and the file skipped (same policy as eager).
        bool registerOneFileShell(const std::filesystem::path& file,
                                  std::shared_ptr<lux::asset::AssetManager> mgr_shared)
        {
            auto shell = lux::asset::makeShellFromFile(
                mgr_shared->codecCatalog(),
                file
            );
            if (!shell.has_value())
            {
                if (shell.error() == lux::asset::EAssetError::UNSUPPORTED)
                    return registerOneFileEager(file, mgr_shared);   // non-lazy type
                std::fprintf(stderr,
                    "[AssetImporter] skip '%s': makeShellFromFile failed (err=%d)\n",
                    file.string().c_str(), static_cast<int>(shell.error()));
                return false;
            }

            // Capture the id before the move so a false return (registerAsset
            // gives bool, not an error code) can be disambiguated: a duplicate
            // on project re-open is success (idempotent), anything else is a
            // genuine failure.
            const lux::asset::asset_id_t id = shell.value()->info()->id;
            if (!mgr_shared->registerAsset(std::move(shell.value())))
            {
                if (mgr_shared->queryInfo(id) != nullptr)
                    return true;   // already present (re-open) — idempotent
                std::fprintf(stderr,
                    "[AssetImporter] skip '%s': registerAsset(shell) failed\n",
                    file.string().c_str());
                return false;
            }
            return true;
        }
    } // namespace

    ImportReport importExternalFileDetached(
        const std::filesystem::path& source,
        const std::filesystem::path& dest_root,
        const ImportOptions& options)
    {
        // 池上专用(批H2):散管理器只为物化+写盘,随本调用消亡 —— 产物是
        // 盘上文件;活账本注册在主线程(registerImportedFiles)。散管理器在
        // 池线程构造,assertLedgerThread 的锚点即池线程,全程自洽。
        auto scratch = std::make_shared<lux::asset::AssetManager>(
            lux::authoring::authoringAssetCodecCatalog()
        );
        return importExternalFile(source, dest_root, std::move(scratch), options);
    }

    std::size_t registerImportedFiles(
        const std::vector<std::filesystem::path>& files,
        std::shared_ptr<lux::asset::AssetManager> manager)
    {
        if (!manager) return 0;
        std::size_t ok = 0;
        for (const auto& f : files)
        {
            const auto ext = f.extension().string();
            if (ext != ".luxasset" && ext != ".luxmodel") continue;
            if (registerOneFileShell(f, manager)) ++ok;
        }
        return ok;
    }

    std::size_t registerContentFolder(
        const std::filesystem::path& content_root,
        std::shared_ptr<lux::asset::AssetManager> manager,
        ELoadMode mode)
    {
        if (!manager) return 0;
        std::error_code ec;
        if (!std::filesystem::exists(content_root, ec) || ec) return 0;

        std::size_t ok = 0;
        for (const auto& de : std::filesystem::recursive_directory_iterator(content_root, ec))
        {
            if (!de.is_regular_file(ec)) continue;
            const auto ext = de.path().extension().string();
            if (ext != ".luxasset" && ext != ".luxmodel") continue;
            const bool done = (mode == ELoadMode::Shells)
                ? registerOneFileShell(de.path(), manager)
                : registerOneFileEager(de.path(), manager);
            if (done) ++ok;
        }
        return ok;
    }

} // namespace lux::editor
