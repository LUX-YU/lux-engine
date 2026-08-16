#include <lux/engine/editor/content/EditorBuiltins.hpp>
#include <lux/engine/toolchain/asset/builtin/BuiltinGeometry.hpp>
#include <lux/engine/editor/content/EngineContentPath.hpp>
#include <lux/engine/editor/content/RuntimeAssetPath.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/MaterialAsset.hpp>
#include <lux/engine/resource/asset/MaterialInstanceAsset.hpp>   // 内置材质的实例包装
#include <lux/engine/resource/asset/MeshAsset.hpp>
#include <lux/engine/resource/asset/MeshSerDeser.hpp>
#include <lux/engine/resource/asset/TextureCodec.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/math/AABB.hpp>

#include <lux/engine/toolchain/asset/material/MaterialGraphCompiler.hpp>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>  // complete type (makeNeutralPbrGraph returns by value)

#include <Eigen/Core>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace lux::editor
{
    using lux::asset::kBuiltinCubeMeshIdStr;
    using lux::asset::kBuiltinEmissiveColors;
    using lux::asset::kBuiltinEmissiveCount;
    using lux::asset::kBuiltinEmissiveIdStrs;
    using lux::asset::kBuiltinEmissiveInstanceIdStrs;
    using lux::asset::kBuiltinMissingMaterialIdStr;
    using lux::asset::kBuiltinPlaneMeshIdStr;
    using lux::asset::kBuiltinPreviewGreyMaterialIdStr;
    using lux::asset::kBuiltinSphereMeshIdStr;
    using lux::asset::kBuiltinWhitePbrInstanceIdStr;
    using lux::asset::kBuiltinWhitePbrMaterialIdStr;
    using lux::toolchain::buildCubeGeometry;
    using lux::toolchain::buildFloorPlaneGeometry;
    using lux::toolchain::buildSphereGeometry;
    using lux::toolchain::compileGraphToPayload;
    using lux::toolchain::flattenMaterialRuntimeValues;
    using lux::toolchain::makeEmissivePbrGraph;
    using lux::toolchain::makeNeutralPbrGraph;

    namespace
    {
        // ── Asset-construction helpers ─────────────────────────────────

        std::unique_ptr<lux::asset::AssetInfo>
        makeInfo(lux::asset::asset_id_t id, lux::asset::EAssetType type)
        {
            auto info  = std::make_unique<lux::asset::AssetInfo>();
            info->id   = id;
            info->type = type;
            info->date = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return info;
        }

        bool parseUuid(const char* s, lux::asset::asset_id_t& out) noexcept
        {
            auto parsed = uuids::uuid::from_string(s);
            if (!parsed) return false;
            out = *parsed;
            return true;
        }

        /// Build an aliasing shared_ptr that lets the SerDeser hold the
        /// editor's AssetManager without ever deleting it (the real owner
        /// is `LuxEditor`).
        std::shared_ptr<lux::asset::AssetManager>
        aliasManagerPtr(lux::asset::AssetManager& mgr)
        {
            return std::shared_ptr<lux::asset::AssetManager>(
                std::shared_ptr<void>{}, &mgr);
        }

        // ── Fallback path: programmatic mesh construction.
        //    Used only when the on-disk engine content is missing. Keeps
        //    the editor bootable in tree-corrupted environments. ─────────

        bool registerCubeFallback(lux::asset::AssetManager& mgr,
                                  lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinCubeMeshIdStr, out_id))
                return false;

            std::vector<lux::rdesc::Vertex> verts;
            std::vector<std::uint32_t>      indices;
            buildCubeGeometry(verts, indices, 0.5f);

            lux::rdesc::Mesh mesh{
                std::move(verts),
                std::move(indices),
                lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f),
                                Eigen::Vector3f( 0.5f,  0.5f,  0.5f))
            };
            auto asset = std::make_unique<lux::asset::MeshAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MESH));
            asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
            return mgr.registerAsset(std::move(asset));
        }

        bool registerPlaneFallback(lux::asset::AssetManager& mgr,
                                   lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinPlaneMeshIdStr, out_id))
                return false;

            std::vector<lux::rdesc::Vertex> verts;
            std::vector<std::uint32_t>      indices;
            buildFloorPlaneGeometry(verts, indices, 5.0f);

            lux::rdesc::Mesh mesh{
                std::move(verts),
                std::move(indices),
                lux::math::AABB(Eigen::Vector3f(-5.0f, -0.01f, -5.0f),
                                Eigen::Vector3f( 5.0f,  0.01f,  5.0f))
            };
            auto asset = std::make_unique<lux::asset::MeshAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MESH));
            asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
            return mgr.registerAsset(std::move(asset));
        }

        bool registerSphereFallback(lux::asset::AssetManager& mgr,
                                    lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinSphereMeshIdStr, out_id))
                return false;

            std::vector<lux::rdesc::Vertex> verts;
            std::vector<std::uint32_t>      indices;
            buildSphereGeometry(verts, indices, 0.5f);

            lux::rdesc::Mesh mesh{
                std::move(verts),
                std::move(indices),
                lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f),
                                Eigen::Vector3f( 0.5f,  0.5f,  0.5f))
            };
            auto asset = std::make_unique<lux::asset::MeshAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MESH));
            asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
            return mgr.registerAsset(std::move(asset));
        }

        // White built-in material — a GRAPH material now (W5 retired rdesc::Material;
        // the editor's sole material path is the node graph). Authored in memory as a
        // neutral white PBR graph + baked to a MaterialData payload, registered
        // under the SAME stable UUID so demo/saved scenes that reference it keep
        // resolving (the bridge dispatches MATERIAL -> ensureGraphMaterial).
        bool registerWhiteGraphMaterial(lux::asset::AssetManager& mgr,
                                        lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinWhitePbrMaterialIdStr, out_id))
                return false;

            auto payload_exp = compileGraphToPayload(
                makeNeutralPbrGraph(1.f, 1.f, 1.f, /*metallic=*/0.f, /*roughness=*/0.6f),
                /*slot_texture_ids=*/{});
            if (!payload_exp)
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] white graph material bake failed: %s\n",
                    payload_exp.error().c_str());
                return false;
            }
            auto payload = std::make_unique<lux::asset::MaterialData>(
                std::move(*payload_exp));
            auto asset = std::make_unique<lux::asset::MaterialAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MATERIAL));
            asset->setData(std::move(payload));
            return mgr.registerAsset(std::move(asset));
        }

        // Preview grey — the neutral 0.78 grey a bare mesh wears in thumbnails /
        // previews. Same bake path as the white material; the recipe mirrors the
        // grey the retired hand-rolled preview scene used to bake at setup() so
        // the SceneRuntime preview paths keep today's look.
        bool registerPreviewGreyMaterial(lux::asset::AssetManager& mgr,
                                         lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinPreviewGreyMaterialIdStr, out_id))
                return false;

            auto payload_exp = compileGraphToPayload(
                makeNeutralPbrGraph(0.78f, 0.78f, 0.80f, /*metallic=*/0.0f, /*roughness=*/0.5f),
                /*slot_texture_ids=*/{});
            if (!payload_exp)
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] preview grey material bake failed: %s\n",
                    payload_exp.error().c_str());
                return false;
            }
            auto asset = std::make_unique<lux::asset::MaterialAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MATERIAL));
            asset->setData(std::make_unique<lux::asset::MaterialData>(
                std::move(*payload_exp)));
            return mgr.registerAsset(std::move(asset));
        }

        // M_Missing — 「资产被删/引用悬空」的醒目兜底(删除扩散裁决七:实体
        // 变色,不消失 —— Unity 粉红/Unreal 灰格的同族选择)。刺眼 magenta,
        // 粗糙度拉满防高光伪装成正常材质。与 PreviewGrey 不同,它**要落盘**
        // (baker 导出 M_Missing.luxasset):player 里资产同样会悬空,纯进程
        // 注册在那里不存在。
        bool registerMissingMaterial(lux::asset::AssetManager& mgr,
                                     lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinMissingMaterialIdStr, out_id))
                return false;

            auto payload_exp = compileGraphToPayload(
                makeNeutralPbrGraph(1.0f, 0.0f, 1.0f, /*metallic=*/0.0f, /*roughness=*/1.0f),
                /*slot_texture_ids=*/{});
            if (!payload_exp)
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] missing-material bake failed: %s\n",
                    payload_exp.error().c_str());
                return false;
            }
            auto asset = std::make_unique<lux::asset::MaterialAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MATERIAL));
            asset->setData(std::make_unique<lux::asset::MaterialData>(
                std::move(*payload_exp)));
            return mgr.registerAsset(std::move(asset));
        }

        // Emissive "bulb" palette — bright unlit-looking glow materials, one per
        // EditorBuiltins palette slot, registered under the shared stable UUIDs so
        // a generated demo scene's light bulbs resolve on load.
        bool registerEmissivePalette(lux::asset::AssetManager& mgr)
        {
            // 八个自发光材质彼此只差三个颜色分量,而颜色走的是 ParamSlotDecl::dflt
            // ——「参数默认值」,不是着色器里的常量:MaterialLowering 把 ParamNode
            // 降低成 ShaderIRValue{op=Param, type, slot},默认值根本不进 IR,只活在
            // cooked parameter_defaults 里。所以八份 SPIR-V 逐字节相同。
            //
            // 此前这里逐个 bake,而每个 bake 编两趟(GBuffer + Forward)—— 每次启动
            // 16 次完整的 glslang 解析 + SPIR-V 生成,其中 14 次的产物是重复的。
            // VTune 实测:shadergen_glsl 占编辑器启动 CPU 的 14.4%(1.05s),是整个
            // 渲染模块 20 秒用量的 6.6 倍,而这一段是其中最大的一块。
            //
            // 改成编一次、复用给全部八个,逐个只换参数默认值。
            // 「SPIR-V 与颜色无关」这条前提由 emissive_palette_spirv_identity_test
            // 钉住 —— 哪天颜色开始进着色器,那个测试会红,而那正是必须改回逐个编
            // 的信号。别把那个测试当可选 smoke 删掉。
            static_assert(kBuiltinEmissiveCount > 0, "调色板不能为空,下面按 [0] 取模板");

            lux::asset::MaterialData baked;
            {
                const float* c0 = kBuiltinEmissiveColors[0];
                auto exp = compileGraphToPayload(
                    makeEmissivePbrGraph(c0[0], c0[1], c0[2]), /*slot_texture_ids=*/{});
                if (!exp)
                {
                    std::fprintf(stderr,
                        "[EditorBuiltins] emissive palette bake failed: %s\n",
                        exp.error().c_str());
                    return false;
                }
                baked = std::move(*exp);
            }

            for (int i = 0; i < kBuiltinEmissiveCount; ++i)
            {
                lux::asset::asset_id_t id{};
                if (!parseUuid(kBuiltinEmissiveIdStrs[i], id))
                    return false;

                const float* c = kBuiltinEmissiveColors[i];

                lux::asset::MaterialData data;
                // 随颜色变的只有扁平参数值。其余全是共享的 bake 产物。
                const auto graph = makeEmissivePbrGraph(c[0], c[1], c[2]);
                flattenMaterialRuntimeValues(graph, data);
                data.gbuffer_spirv    = baked.gbuffer_spirv;
                data.gbuffer_info     = baked.gbuffer_info;
                data.forward_spirv    = baked.forward_spirv;
                data.forward_info     = baked.forward_info;
                data.texture_slot_ids = baked.texture_slot_ids;

                auto asset = std::make_unique<lux::asset::MaterialAsset>(
                    makeInfo(id, lux::asset::EAssetType::MATERIAL));
                asset->setData(std::make_unique<lux::asset::MaterialData>(std::move(data)));
                if (!mgr.registerAsset(std::move(asset)))
                {
                    std::fprintf(stderr,
                        "[EditorBuiltins] emissive material %d register failed\n", i);
                    return false;
                }
            }
            return true;
        }

        /// 给一个内置材质注册「空覆盖」的实例包装。
        ///
        /// 编辑器的材质槽只收 MATERIAL_INSTANCE,而内置材质都是 MATERIAL —— 没有
        /// 包装就一个也挂不上。包装体不带覆盖(两个掩码都是 0),渲染结果与直接挂
        /// 父级完全一致;用户真要改哪一项时,改的是自己这份实例,不会波及兄弟实体。
        ///
        /// 只注册进内存资产表,不落盘:它们是**进程级内置**,与 cube/plane/sphere
        /// 同级,UUID 固定所以场景引用得住;写进用户的 Content 目录反而会让
        /// "内置"和"项目资产"混在一起。
        bool registerInstanceWrapper(lux::asset::AssetManager&     mgr,
                                     const char*                   id_str,
                                     const lux::asset::asset_id_t& parent_id,
                                     const char*                   display_name,
                                     lux::asset::asset_id_t&       out_id)
        {
            if (!parseUuid(id_str, out_id))
                return false;

            auto data = std::make_unique<lux::asset::MaterialInstanceData>();
            data->parent_material_id = parent_id;   // 覆盖掩码保持 0 = 全部继承

            auto asset = std::make_unique<lux::asset::MaterialInstanceAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MATERIAL_INSTANCE));
            asset->setData(std::move(data));
            if (auto* mi = asset->mutableInfo())
            {
                const std::size_t n =
                    std::min(std::strlen(display_name), sizeof(mi->display_name) - 1);
                std::memcpy(mi->display_name, display_name, n);
                mi->display_name[n] = '\0';
            }
            if (!mgr.registerAsset(std::move(asset)))
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] instance wrapper '%s' register failed\n", display_name);
                return false;
            }
            return true;
        }

        // ── Primary path: load a .luxasset from disk via SerDeser. ──────

        template <class SerDeser>
        bool tryLoadFromDisk(SerDeser&                     ser,
                             const std::filesystem::path&  path,
                             lux::asset::asset_id_t&       out_id,
                             const char*                   label)
        {
            namespace fs = std::filesystem;
            if (!fs::exists(path))
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] %s missing at '%s' - using fallback.\n",
                    label, path.string().c_str());
                return false;
            }
            auto res = ser.fromLuxAsset(path);
            if (!res)
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] %s load failed (code=%d) - using fallback.\n",
                    label, static_cast<int>(res.error()));
                return false;
            }
            out_id = res.value().second;
            return true;
        }
    } // namespace

    bool EditorBuiltins::registerInto(lux::asset::AssetManager& mgr)
    {
        if (ready_) return true;

        namespace fs = std::filesystem;
        const auto content_root = engine_content_path;

        auto mgr_ref = aliasManagerPtr(mgr);
        lux::asset::MeshSerDeser mesh_ser(mgr_ref);

        // ── Cube ───────────────────────────────────────────────────────
        if (!tryLoadFromDisk(mesh_ser,
                             content_root / "Meshes" / "SM_Cube.luxasset",
                             cube_mesh_id_,
                             "cube"))
        {
            if (!registerCubeFallback(mgr, cube_mesh_id_))
            {
                std::fprintf(stderr, "[EditorBuiltins] cube fallback failed\n");
                return false;
            }
        }

        // ── Plane ──────────────────────────────────────────────────────
        if (!tryLoadFromDisk(mesh_ser,
                             content_root / "Meshes" / "SM_Plane.luxasset",
                             plane_mesh_id_,
                             "plane"))
        {
            if (!registerPlaneFallback(mgr, plane_mesh_id_))
            {
                std::fprintf(stderr, "[EditorBuiltins] plane fallback failed\n");
                return false;
            }
        }

        // ── Sphere (material-preview ball) ─────────────────────────────
        if (!tryLoadFromDisk(mesh_ser,
                             content_root / "Meshes" / "SM_Sphere.luxasset",
                             sphere_mesh_id_,
                             "sphere"))
        {
            if (!registerSphereFallback(mgr, sphere_mesh_id_))
            {
                std::fprintf(stderr, "[EditorBuiltins] sphere fallback failed\n");
                return false;
            }
        }

        // ── White (graph) material ─────────────────────────────────────
        //
        // Baked in memory from a neutral white PBR graph (W5: rdesc::Material is
        // retired, so the built-in white material is a GRAPH material like every
        // other). Disk-load is intentionally absent, and the stable UUID keeps
        // demo/saved scenes resolving.
        //
        // 注意「bake 很便宜」这个说法不成立:VTune 实测整个 registerInto 的着色器
        // 编译是编辑器启动期最大的一项 CPU 开销。自发光调色板那部分已经靠去重消掉
        // (见下),剩下的就是这一次白材质 bake —— 两趟 glslang,约占原开销的六分之一。
        // 它没法去重(图与调色板不同),真要再降只能上磁盘缓存或挪进线程池。
        if (!registerWhiteGraphMaterial(mgr, white_pbr_id_))
        {
            std::fprintf(stderr, "[EditorBuiltins] white graph material registration failed\n");
            return false;
        }

        // ── Preview grey material(缩略图/预览的裸网格默认材质)────────────
        if (!registerPreviewGreyMaterial(mgr, preview_grey_id_))
        {
            std::fprintf(stderr, "[EditorBuiltins] preview grey material registration failed\n");
            return false;
        }

        // ── M_Missing(资产悬空的醒目兜底,裁决七)──────────────────────────
        {
            lux::asset::asset_id_t missing_id{};
            if (!registerMissingMaterial(mgr, missing_id))
            {
                std::fprintf(stderr, "[EditorBuiltins] missing material registration failed\n");
                return false;
            }
        }

        // ── Emissive bulb palette (visible light sources for demo lights) ──
        if (!registerEmissivePalette(mgr))
        {
            std::fprintf(stderr, "[EditorBuiltins] emissive palette registration failed\n");
            return false;
        }

        // ── 实例包装:内置材质在编辑器里能被挂上的那一份 ──────────────
        //
        // 材质槽只收 MATERIAL_INSTANCE(见 MeshComponent::material_asset_id 的
        // asset_type 注解),而上面注册的九个都是 MATERIAL。没有这一段,内置材质
        // 在 Inspector 里一个也选不到 —— 收紧材质槽的代价必须在这里补上,否则
        // "开箱即用"就只剩一句话。
        if (!registerInstanceWrapper(mgr, kBuiltinWhitePbrInstanceIdStr,
                                     white_pbr_id_, "WhitePBR_Inst", white_pbr_inst_id_))
            return false;

        for (int i = 0; i < kBuiltinEmissiveCount; ++i)
        {
            lux::asset::asset_id_t parent{};
            if (!parseUuid(kBuiltinEmissiveIdStrs[i], parent))
                return false;

            char name[32];
            std::snprintf(name, sizeof(name), "Emissive%d_Inst", i);

            lux::asset::asset_id_t inst{};   // 调用方不需要留着,场景按 UUID 引用
            if (!registerInstanceWrapper(mgr, kBuiltinEmissiveInstanceIdStrs[i],
                                         parent, name, inst))
                return false;
        }

        // ── Skybox texture ─────────────────────────────────────────────
        //
        // Lives in the render module's runtime-asset tree (unchanged from
        // M1.5). The UUID inside the file is whatever was baked there; we
        // record what the SerDeser reports. Missing file is non-fatal —
        // the Skybox bridge sees a nil UUID and pushes nothing.
        {
            const fs::path sky_path =
                fs::path(editor_runtime_asset_path) / "textures" / "blue_nebulae_1.luxasset";

            if (fs::exists(sky_path))
            {
                lux::asset::TextureCodec tex_ser(mgr_ref);
                auto tex_res = tex_ser.fromLuxAsset(sky_path);
                if (!tex_res)
                {
                    std::fprintf(stderr,
                        "[EditorBuiltins] skybox texture load failed (code=%d)\n",
                        static_cast<int>(tex_res.error()));
                }
                else
                {
                    skybox_tex_id_ = tex_res.value().second;
                }
            }
            else
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] skybox texture missing at %s - sky will be empty\n",
                    sky_path.string().c_str());
            }
        }

        ready_ = true;
        return true;
    }

} // namespace lux::editor
