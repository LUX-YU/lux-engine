// ============================================================================
//  material_residency_gpu_probe — 驻留 T9/T10 端到端探针(真设备往返)。
//
//  三件套 + 真材质双域子服务打通最复杂的两域:烘焙图材质(编译 gbuffer/
//  forward → 组装上传)与链式材质实例(父有效 ⊕ override,根 shader 共享
//  PSO)。放在编辑器测试层:烘焙入口(compileGraphToPayload)是编辑器
//  私有 src 的能力。
//
//  依赖门走真路径:实例 ensure → 编排申报父级依赖 → 自动 ensure 父材质
//  (整条编译链)→ 结算后实例才 submit。无 Vulkan = 跳过(exit 0)。
//  validation 全程开,含销毁期零错误(根 shader 恰在根销毁时归还,
//  实例绝不 destroyShader —— 错了这里就报双重销毁)。
// ============================================================================

#include "DeviceRenderFixture.hpp"
#include <lux/engine/toolchain/asset/material/MaterialGraphCompiler.hpp>

#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceOps.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/subservices/MaterialSubservices.hpp>

#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialInstanceAsset.hpp>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>

#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>   // kMaterialFeatureFactory

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <span>
#include <thread>

using lux::asset::asset_id_t;
namespace ops = lux::runtime::render_resource;

static int g_fail = 0;
static void check(bool c, const char* what)
{
    std::printf("%s %s\n", c ? "[ ok ]" : "[FAIL]", what);
    if (!c) ++g_fail;
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 探针可能挂死:逐行即见
    std::atomic<int> validation_errors{0};
    {
        lux::rendertest::DeviceRenderFixture fx(
            64, 64, "material_residency_gpu_probe",
            {.enable_validation = true, .validation_errors = &validation_errors});
        if (!fx.ok())
        {
            std::printf("[skip] no Vulkan device — "
                        "material_residency_gpu_probe skipped\n");
            return 0;
        }

        // 特性注册 + attach(材质上传 op 经进程域目录取)。
        lux::render::FeatureCatalog catalog;
        const auto sv      = fx.makeSceneWithView();
        const auto mat_reg = fx.awaitControl(fx.control().registerFeatureType(
            lux::render::kMaterialFeatureFactory));
        catalog.add(lux::render::kMaterialFeatureFactory,
                    mat_reg.feature_type_id,
                    std::span<const lux::render::TypeId>(mat_reg.ops,
                                                         mat_reg.op_count));
        fx.awaitControl(fx.control().addFeature(sv.scene_id, mat_reg.feature_type_id,
                                         lux::render::MaterialCommTag{}));

        lux::asset::AssetManager                 mgr{
            lux::asset::runtimeAssetCodecCatalog()};
        lux::runtime::testing::AsyncTestServices async(
            mgr,
            fx.upload(),
            fx.sync(),
            lux::exec::AsyncRuntimeConfig{
                .blocking_io_threads = 1,
                .background_cpu_concurrency = 1}
        );
        check(async.valid(), "async test services assembled");
        lux::runtime::MaterialArtifactStore      store;
        lux::runtime::RenderResourceService      service;
        lux::runtime::RenderResourceStateManager table;
        lux::exec::AsyncScope                    material_tasks(async.runtime());
        auto lookup = [&table](const asset_id_t& rid) -> std::uint64_t
        {
            using EState = lux::runtime::RenderResourceStateManager::EState;
            const auto* r = table.find(rid);
            return (r != nullptr && r->state == EState::READY)
                       ? r->resident.bits()
                       : 0;
        };
        lux::runtime::TryPostToMain post_main =
            [main = async.runtime().mainThreadDispatcher()](
                lux::cxx::move_only_function<void()> task) noexcept
            { return main.tryDispatchToMainThread(std::move(task)); };
        service.addSubservice(
            std::make_unique<lux::runtime::MaterialSubservice>(
                fx.control(),
                async.uploadClient(),
                mgr,
                catalog,
                store,
                lookup,
                post_main,
                material_tasks
            ));
        service.addSubservice(
            std::make_unique<lux::runtime::MaterialInstanceSubservice>(
                fx.control(), async.uploadClient(), mgr, catalog, store, lookup,
                std::move(post_main)));
        ops::Context ctx{
            table,
            service,
            mgr,
            async.assetClient(),
            material_tasks,
            async.runtime(),
            {},
            {}
        };

        using EState = lux::runtime::RenderResourceStateManager::EState;

        // ── 烘焙一个图材质入账(纯 CPU 烘焙:lower + shaderc)──────────
        auto payload = lux::toolchain::compileGraphToPayload(
            lux::toolchain::makeEmissivePbrGraph(1.0f, 0.2f, 0.1f),
            /*slot_texture_ids=*/{});
        if (!payload)
        {
            std::printf("[FAIL] bake failed: %s\n", payload.error().c_str());
            return 1;
        }
        auto minfo  = std::make_unique<lux::asset::AssetInfo>();
        minfo->id   = mgr.generateUUID();
        minfo->type = lux::asset::MaterialAsset::asset_type;
        const auto mat_id = minfo->id;
        auto masset = std::make_unique<lux::asset::MaterialAsset>(
            std::move(minfo));
        masset->setData(std::make_unique<lux::asset::MaterialData>(
            std::move(*payload)));
        mgr.registerAsset(std::move(masset));

        // 宿主安全点同时泵 frame/control/upload 三条 reply lane。材质的
        // CPU 编译续体由 stdexec/MainThreadScheduler 推进，最终持久 GPU 数据独立走
        // UploadChannel，不再依赖 frame 是否 OPEN。
        auto pumpUntil = [&](auto&& settled)
        {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto progress = fx.session().observeProgress();
                fx.pumpReplies();
                async.drainMainThreadCompletions();
                if (settled()) return true;
                fx.session().trySubmitFrame();
                fx.window().pollEvents();
                fx.session().beginFrame({});
                (void)fx.session().waitForProgressUntil(
                    progress,
                    std::min(
                        deadline,
                        std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(10))
                );
            }
            return settled();
        };
        auto closeScope = [&](lux::exec::AsyncScope& scope)
        {
            std::atomic<bool> closed{false};
            lux::exec::detail::subscribeScopeClose(
                scope,
                [&closed]() noexcept
                {
                    closed.store(true, std::memory_order_release);
                    closed.notify_one();
                });
            return pumpUntil(
                [&closed]() noexcept
                {
                    return closed.load(std::memory_order_acquire);
                });
        };
        auto pumpSettled = [&](const asset_id_t& rid)
        {
            (void)pumpUntil([&]
            {
                const auto* r = table.find(rid);
                return r != nullptr && (r->state == EState::READY
                                        || r->state == EState::FAILED);
            });
        };

        // ── T9:图材质整条编译链(gbuffer → forward → 上传)────────────
        ops::ensure(ctx, mat_id,
                    lux::ecs::EResourceDomain::MATERIAL);
        pumpSettled(mat_id);
        check(table.find(mat_id)->state == EState::READY
                  && table.find(mat_id)->resident.bits() != 0,
              "T9: 图材质编译+上传 → READY");
        const auto mat_bits = table.find(mat_id)->resident.bits();
        const auto mat_artifact = store.by_handle_bits.find(mat_bits);
        check(mat_artifact != store.by_handle_bits.end()
                  && mat_artifact->second.owns_shaders,
              "T9: 根材质按精确 handle bits 记账且拥有 shader");

        // 同一 asset id 再上传一代并让两代短暂并存。旧的 id→artifact
        // 账本会在这里覆盖第一代,随后销毁第二代时误删/误收第一代;
        // handle bits 键必须能精确摘除第二代而保留表内第一代。
        std::uint64_t second_generation_bits{0};
        bool second_generation_settled{false};
        bool second_generation_failed{false};
        const bool second_generation_submitted = service.submit(
            lux::ecs::EResourceDomain::MATERIAL,
            mat_id,
            [&](std::uint64_t bits, std::string_view fail)
            {
                second_generation_bits    = bits;
                second_generation_failed  = !fail.empty();
                second_generation_settled = true;
            }
        );
        check(second_generation_submitted,
              "T9/ABA: 同 id 第二代请求已提交");
        const bool second_generation_pumped = pumpUntil(
            [&] { return second_generation_settled; }
        );
        check(second_generation_pumped && !second_generation_failed
                  && second_generation_bits != 0
                  && second_generation_bits != mat_bits
                  && store.by_handle_bits.contains(mat_bits)
                  && store.by_handle_bits.contains(second_generation_bits),
              "T9/ABA: 同 id 两代 GPU 句柄可同时驻留");
        {
            auto second_generation = service.adoptHandle(
                lux::ecs::EResourceDomain::MATERIAL,
                second_generation_bits
            );
            check(static_cast<bool>(second_generation),
                  "T9/ABA: 第二代句柄可转入唯一 RAII lease");
        }
        check(store.by_handle_bits.contains(mat_bits)
                  && !store.by_handle_bits.contains(second_generation_bits),
              "T9/ABA: 销毁第二代不会误删第一代 artifact");

        // ── T10:实例挂父材质(依赖门自动带父到结算)+ 链式实例 ────────
        auto registerInstance = [&](const asset_id_t& parent)
        {
            auto data                = std::make_unique<
                lux::asset::MaterialInstanceData>();
            data->parent_material_id = parent;
            data->param_override_mask = 1u;   // override 参数道 0
            data->params[0][0] = 0.1f;
            data->params[0][1] = 0.9f;
            data->params[0][2] = 0.1f;
            data->params[0][3] = 1.0f;
            auto info  = std::make_unique<lux::asset::AssetInfo>();
            info->id   = mgr.generateUUID();
            info->type = lux::asset::MaterialInstanceAsset::asset_type;
            const auto id = info->id;
            auto a = std::make_unique<lux::asset::MaterialInstanceAsset>(
                std::move(info));
            a->setData(std::move(data));
            mgr.registerAsset(std::move(a));
            return id;
        };

        const auto inst_id = registerInstance(mat_id);
        ops::ensure(ctx, inst_id,
                    lux::ecs::EResourceDomain::MATERIAL_INSTANCE);
        pumpSettled(inst_id);
        check(table.find(inst_id)->state == EState::READY
                  && table.find(inst_id)->resident.bits() != 0
                  && table.find(inst_id)->resident.bits()
                         != table.find(mat_id)->resident.bits(),
              "T10: 实例 READY(自有材质句柄,父 shader 共享)");
        check(!table.find(inst_id)->depends_on.empty()
                  && table.find(inst_id)->depends_on[0] == mat_id,
              "T10: 父级依赖边入表(级联/票据在位)");
        const auto inst_bits = table.find(inst_id)->resident.bits();
        const auto inst_artifact = store.by_handle_bits.find(inst_bits);
        check(inst_artifact != store.by_handle_bits.end()
                  && !inst_artifact->second.owns_shaders,
              "T10: 实例按自身 handle bits 记账且不拥有父 shader");

        // 链式:实例的实例(根 shader 逐级抄下)。
        const auto inst2_id = registerInstance(inst_id);
        ops::ensure(ctx, inst2_id,
                    lux::ecs::EResourceDomain::MATERIAL_INSTANCE);
        pumpSettled(inst2_id);
        check(table.find(inst2_id)->state == EState::READY
                  && table.find(inst2_id)->resident.bits() != 0,
              "T10: 链式实例(实例的实例)READY");
        const auto inst2_bits = table.find(inst2_id)->resident.bits();
        const auto inst2_artifact = store.by_handle_bits.find(inst2_bits);
        check(inst2_artifact != store.by_handle_bits.end()
                  && !inst2_artifact->second.owns_shaders
                  && store.by_handle_bits.size() == 3,
              "T10: 根/实例/链式实例三代句柄可同时精确记账");

        // ── 力扫:实例→材质依赖序;根 shader 恰一次归还 ────────────────
        ops::teardown(ctx);
        fx.flush(3);
        check(table.size() == 0, "力扫后表空(实例先于材质)");
        check(store.by_handle_bits.empty(),
              "力扫后材质域句柄账本为空");
        check(closeScope(material_tasks),
              "material owner scope closes through its sender state");

        // ── owner stop:不可取消叶子移交 reaper，迟到 handle 自动补偿 ──
        {
            lux::runtime::MaterialArtifactStore stopped_store;
            lux::runtime::RenderResourceService stopped_service;
            lux::exec::AsyncScope               stopped_tasks(async.runtime());
            auto stopped_material =
                std::make_unique<lux::runtime::MaterialSubservice>(
                    fx.control(),
                    async.uploadClient(),
                    mgr,
                    catalog,
                    stopped_store,
                    lookup,
                    [main = async.runtime().mainThreadDispatcher()](
                        lux::cxx::move_only_function<void()> task) noexcept
                    {
                        return main.tryDispatchToMainThread(std::move(task));
                    },
                    stopped_tasks
                );
            stopped_service.addSubservice(std::move(stopped_material));

            int stopped_completions = 0;
            std::uint64_t stopped_bits = 0;
            bool stopped_failed = false;
            check(stopped_service.submit(
                      lux::ecs::EResourceDomain::MATERIAL,
                      mat_id,
                      [&](std::uint64_t bits, std::string_view fail)
                      {
                          ++stopped_completions;
                          stopped_bits   = bits;
                          stopped_failed = !fail.empty();
                      }
                  ),
                  "stop:三段材质 sender 已进入 owner scope");

            // 请求已被 UploadChannel 受理；先 stop sender owner，再由
            // progress 泵独立 reply lane，让不可取消的 server work 收尾。
            check(closeScope(stopped_tasks),
                  "stopped owner scope closes without synchronous self-wait");
            check(stopped_completions == 1 && stopped_bits == 0
                      && stopped_failed,
                  "stop:外层完成恰一次且不发布 READY handle");

            const bool late_reply_reaped = pumpUntil(
                [&]
                {
                    return stopped_service.pendingReplies(
                        lux::ecs::EResourceDomain::MATERIAL
                    ) == 0;
                }
            );
            check(late_reply_reaped && stopped_completions == 1,
                  "stop:迟到回执由 reaper 消费且不产生第二终态");
            check(stopped_store.by_handle_bits.empty(),
                  "stop:迟到 owner 仅补偿，不污染 material store");
            fx.flush(2);   // 提交 transaction 析构产生的 deferred destroy
        }
    }
    check(validation_errors.load() == 0,
          "validation 零错误(含销毁期:shader 恰一次归还)");

    std::printf(g_fail == 0 ? "\nALL CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n",
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
