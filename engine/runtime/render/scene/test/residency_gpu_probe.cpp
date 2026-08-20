// ============================================================================
//  residency_gpu_probe — 驻留域子服务端到端探针(真设备往返;T7 起逐域扩容)。
//
//  纯状态表 + 全局服务 + 编排管道 + **真域子服务**打通:ensure → 命中直通
//  上传段 → 服务器回执 → READY 行 + 等待送达 → 力扫销毁。validation 全程
//  开,含销毁期零错误。无 Vulkan 设备 = 跳过(exit 0)。
//
//  覆盖域:TEXTURE(createTexture2D 核心协议,mip 生成路)、
//          MESH(uploadMesh 特性动态 op,目录按名字取)。
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceOps.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/subservices/TextureSubservice.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/subservices/MeshSubservice.hpp>

#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/TextureAsset.hpp>
#include <lux/engine/resource/asset/MeshAsset.hpp>

#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>   // kMaterialFeatureFactory
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>  // kMeshStackFeatureFactory
#include <lux/cxx/core/Format.hpp>                        // lux::format

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

using lux::asset::asset_id_t;
namespace ops = lux::runtime::render_resource;

static int g_fail = 0;
static void check(bool c, const char* what)
{
    std::printf("%s %s\n", c ? "[ ok ]" : "[FAIL]", what);
    if (!c) ++g_fail;
}

/// 4×4 RGBA8 程序化贴图直接入账(命中路;不带 NO_MIPS —— 走 mip 生成)。
static asset_id_t registerRgba8(lux::asset::AssetManager& mgr)
{
    constexpr std::uint32_t W = 4, H = 4;
    std::vector<std::byte>  px(static_cast<std::size_t>(W) * H * 4,
                               std::byte{0x7F});
    lux::rdesc::TextureInfo ti{};
    ti.width        = W;
    ti.height       = H;
    ti.channel      = 4;
    ti.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
    ti.mip_count    = 1;
    ti.layers       = 1;
    auto info  = std::make_unique<lux::asset::AssetInfo>();
    info->id   = mgr.generateUUID();
    info->type = lux::asset::TextureAsset::asset_type;
    const auto id = info->id;
    auto a = std::make_unique<lux::asset::TextureAsset>(std::move(info));
    auto texture = lux::rdesc::Texture::copyOf(ti, px);
    if (!texture)
        return {};
    a->setData(std::make_unique<lux::rdesc::Texture>(std::move(*texture)));
    mgr.registerAsset(std::move(a));
    return id;
}

/// 4×4 + 2×2 + 1×1 cooked mip chain. The runtime must preserve these
/// immutable ranges so Residency can rebuild a stable texture slot at any
/// logical mip base without retaining a backend CPU copy.
static asset_id_t registerRgba8Mips(lux::asset::AssetManager& mgr)
{
    std::vector<std::byte> pixels(64u + 16u + 4u, std::byte{0x5f});
    lux::rdesc::TextureInfo info{};
    info.width = 4;
    info.height = 4;
    info.channel = 4;
    info.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
    info.mip_count = 3u;
    info.layers = 1u;
    info.mip_ranges[0] = {0u, 64u, 4u, 4u};
    info.mip_ranges[1] = {64u, 16u, 2u, 2u};
    info.mip_ranges[2] = {80u, 4u, 1u, 1u};
    auto asset_info = std::make_unique<lux::asset::AssetInfo>();
    asset_info->id = mgr.generateUUID();
    asset_info->type = lux::asset::TextureAsset::asset_type;
    const auto id = asset_info->id;
    auto asset = std::make_unique<lux::asset::TextureAsset>(
        std::move(asset_info));
    auto texture = lux::rdesc::Texture::copyOf(info, pixels);
    if (!texture)
        return {};
    asset->setData(std::make_unique<lux::rdesc::Texture>(
        std::move(*texture)));
    mgr.registerAsset(std::move(asset));
    return id;
}

/// 单三角程序化网格直接入账。
static asset_id_t registerTriangle(lux::asset::AssetManager& mgr)
{
    std::vector<lux::rdesc::Vertex> verts(3);
    verts[0].position = {0.f, 0.f, 0.f};
    verts[1].position = {1.f, 0.f, 0.f};
    verts[2].position = {0.f, 1.f, 0.f};
    for (auto& v : verts) { v.normal = {0.f, 0.f, 1.f}; v.uv = {0.f, 0.f}; }
    std::vector<std::uint32_t> idx{0, 1, 2};
    auto mesh = std::make_unique<lux::rdesc::Mesh>(lux::rdesc::Mesh{
        std::move(verts), std::move(idx),
        lux::math::AABB(Eigen::Vector3f(0.f, 0.f, 0.f),
                        Eigen::Vector3f(1.f, 1.f, 0.f))});
    auto info  = std::make_unique<lux::asset::AssetInfo>();
    info->id   = mgr.generateUUID();
    info->type = lux::asset::MeshAsset::asset_type;
    const auto id = info->id;
    auto a = std::make_unique<lux::asset::MeshAsset>(std::move(info));
    a->setData(std::move(mesh));
    mgr.registerAsset(std::move(a));
    return id;
}

int main()
{
    std::atomic<int> validation_errors{0};
    {
        lux::rendertest::DeviceRenderFixture fx(
            64, 64, "residency_gpu_probe",
            {.enable_validation = true, .validation_errors = &validation_errors});
        if (!fx.ok())
        {
            std::printf("[skip] no Vulkan device — residency_gpu_probe skipped\n");
            return 0;
        }

        // ── 特性注册 + attach(Material 先于 MeshStack,attach 序约束)──
        // 上传 op 是特性动态 op:注册回执进程域目录;attach 让服务端场景
        // 资源就位。句柄是场景域状态,上传/销毁不需要 —— 目录即足够。
        lux::render::FeatureCatalog catalog;
        const auto sv      = fx.makeSceneWithView();
        const auto mat_reg = fx.awaitControl(fx.control().registerFeatureType(lux::render::kMaterialFeatureFactory));
        const auto ms_reg  = fx.awaitControl(fx.control().registerFeatureType(lux::render::kMeshStackFeatureFactory));
        catalog.add(lux::render::kMeshStackFeatureFactory, ms_reg.feature_type_id,
                    std::span<const lux::render::TypeId>(ms_reg.ops,
                                                         ms_reg.op_count));
        fx.awaitControl(fx.control().addFeature(sv.scene_id, mat_reg.feature_type_id,
                                         lux::render::MaterialCommTag{}));
        fx.awaitControl(fx.control().addFeature(sv.scene_id, ms_reg.feature_type_id,
                                         lux::render::MeshStackCommTag{}));

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
        lux::runtime::RenderResourceService      service;
        lux::runtime::RenderResourceStateManager table;
        lux::exec::AsyncScope                    residency_tasks(async.runtime());
        lux::runtime::TryPostToMain post_main =
            [main = async.runtime().mainThreadDispatcher()](
                lux::cxx::move_only_function<void()> task) noexcept
            { return main.tryDispatchToMainThread(std::move(task)); };
        auto texture_subservice =
            std::make_unique<lux::runtime::TextureSubservice>(
                fx.control(),
                async.uploadClient(),
                mgr,
                post_main,
                lux::runtime::TextureStreamingBudget{
                    .maximum_replacement_tasks = 1u});
        auto* texture_streaming = texture_subservice.get();
        service.addSubservice(std::move(texture_subservice));
        service.addSubservice(std::make_unique<lux::runtime::MeshSubservice>(
            fx.control(), async.uploadClient(), mgr, catalog,
            std::move(post_main)));
        ops::Context ctx{
            table,
            service,
            mgr,
            async.assetClient(),
            residency_tasks,
            async.runtime(),
            {},
            {}
        };

        using EState = lux::runtime::RenderResourceStateManager::EState;

        /// ensure → 泵到 READY,校验送达与表一致。
        auto bringUp = [&](const asset_id_t& id, lux::ecs::EResourceDomain d,
                           const char* tag) -> std::uint64_t
        {
            std::uint64_t delivered = 0;
            bool          failed    = false;
            auto wait = service.await(id,
                [&](std::uint64_t bits, const lux::ecs::ResourceFailure* f)
                {
                    if (f != nullptr) failed = true;
                    else delivered = bits;
                });
            ops::ensure(ctx, id, d);
            check(table.find(id) != nullptr
                      && table.find(id)->state == EState::UPLOADING,
                  lux::format("{}: 命中路直通上传段(UPLOADING)", tag).c_str());
            for (int i = 0;
                 i < 60 && table.find(id)->state == EState::UPLOADING; ++i)
            {
                async.drainMainThreadCompletions();
                fx.flush();   // 提交帧 + 泵回执 → SubmitDone → markReady
            }
            check(table.find(id)->state == EState::READY,
                  lux::format("{}: 真实上传回执 → READY", tag).c_str());
            check(!failed && delivered != 0
                      && table.find(id)->resident.bits() == delivered,
                  lux::format("{}: 送达句柄与表一致且非空", tag).c_str());
            return delivered;
        };

        bringUp(registerRgba8(mgr),    lux::ecs::EResourceDomain::TEXTURE,
                "TEXTURE");
        bringUp(registerTriangle(mgr), lux::ecs::EResourceDomain::MESH,
                "MESH");

        const auto streamed_id = registerRgba8Mips(mgr);
        const auto streamed_bits = bringUp(
            streamed_id,
            lux::ecs::EResourceDomain::TEXTURE,
            "TEXTURE_MIPS");
        const auto streamed_handle = lux::ecs::unpackHandleBits<
            lux::render::RTextureHandle>(streamed_bits);
        const auto deferred_streamed_bits = bringUp(
            registerRgba8Mips(mgr),
            lux::ecs::EResourceDomain::TEXTURE,
            "TEXTURE_MIPS_DEFERRED");
        const auto deferred_streamed_handle = lux::ecs::unpackHandleBits<
            lux::render::RTextureHandle>(deferred_streamed_bits);
        lux::render::TextureMipDemandsReply downgrade{};
        downgrade.count = 2u;
        downgrade.entries[0] = {
            streamed_handle,
            0u,
            1u};
        downgrade.entries[1] = {
            deferred_streamed_handle,
            0u,
            1u};
        texture_streaming->applyMipDemands(downgrade);
        check(
            texture_streaming->pendingReplies() == 1u,
            "TEXTURE_MIPS: per-tick replacement task budget enforced");
        for (int i = 0;
             i < 60 && texture_streaming->pendingReplies() != 0u;
             ++i)
        {
            async.drainMainThreadCompletions();
            fx.flush();
        }
        check(
            texture_streaming->pendingReplies() == 0u,
            "TEXTURE_MIPS: Residency mip-range replacement settled");
        const auto restore_demand = fx.awaitControl(
            fx.control().request<lux::render::TextureMipDemandsReply>(
                lux::render::opcodes::ResourceOp,
                lux::render::type_ids::QueryTextureMipDemands,
                lux::render::QueryTextureMipDemandsPayload{}));
        const auto restore = std::find_if(
            restore_demand.entries.begin(),
            restore_demand.entries.begin() + restore_demand.count,
            [&](const lux::render::TextureMipDemandEntry& demand)
            {
                return demand.handle.index == streamed_handle.index &&
                    demand.handle.gen == streamed_handle.gen;
            });
        check(
            restore != restore_demand.entries.begin() + restore_demand.count &&
                restore->resident_base_mip == 1u &&
                restore->target_base_mip == 0u,
            "TEXTURE_MIPS: backend reports edge demand after coarse replacement");
        if (restore != restore_demand.entries.begin() + restore_demand.count)
        {
            lux::render::TextureMipDemandsReply restore_batch{};
            restore_batch.count = 1u;
            restore_batch.entries[0] = *restore;
            texture_streaming->applyMipDemands(restore_batch);
            for (int i = 0;
                 i < 60 && texture_streaming->pendingReplies() != 0u;
                 ++i)
            {
                async.drainMainThreadCompletions();
                fx.flush();
            }
        }
        check(
            texture_streaming->pendingReplies() == 0u,
            "TEXTURE_MIPS: full mip chain restoration settled");

        ops::teardown(ctx);   // 力扫 → destroyHandle → destroyTexture/destroyMesh
        fx.flush(2);          // 销毁命令上线
        check(table.size() == 0, "力扫后表空");
        lux::exec::testing::closeScope(
            residency_tasks,
            async.runtime()
        );
    }
    check(validation_errors.load() == 0, "validation 零错误(含销毁期)");

    std::printf(g_fail == 0 ? "\nALL CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n",
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
