// ============================================================================
//  instance_bridge_invalid_reply_test.cpp — runtime (headless, GPU-free) coverage
//  of InstanceBridge failed-create handling, over a fake in-process render server
//  (HeadlessBridgeFixture). Two Gate -1 scenarios:
//
//   Scenario A — G-05 (no per-frame retry of a permanent failure): the server
//     replies InvalidConfiguration; the bridge records it in failed_ and does NOT
//     re-issue addMeshInstance every frame (count stays 1 across further drives).
//
//   Scenario B — P1-2 (a DEFAULT/Unknown status must not be read as Ok): the server
//     replies with the DEFAULT status (Unknown) but a NON-null object — the real
//     shape of a generic dispatch failure (RenderRequest delivers a default reply).
//     The status default was the bug: it used to be Ok, so such a reply became a
//     zombie live instance. This scenario forces a valid object so the `!r.object`
//     guard half cannot mask the status check, then proves the instance never went
//     live by removing the entity and asserting reap emits NO RemoveMeshInstance
//     (a live instance would emit one). Under the pre-fix default-Ok, the instance
//     WOULD be live and reap WOULD remove it, flipping the assertion.
//
//  The upload path is real: ensureMesh resolves a minimal in-memory MeshAsset and
//  fires UploadMesh; only after its (successful) reply does the bridge reach
//  addMeshInstance. material_asset_id stays nil (ensureMaterial short-circuits).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/gameplay/render_bridge/InstanceBridge.hpp>
#include <lux/engine/gameplay/3d/traits/MeshRenderTraits.hpp>

#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdio>
#include <memory>

namespace
{
    int g_failures = 0;

    void check(bool cond, const char* msg)
    {
        std::printf("[%s] %s\n", cond ? " ok " : "FAIL", msg);
        if (!cond) ++g_failures;
    }

    using lux::gameplay::d3::MeshComponent;
    using lux::gameplay::d3::WorldTransformComponent;

    lux::asset::asset_id_t makeMeshAsset(lux::bridgetest::HeadlessBridgeFixture& fix)
    {
        auto a = fix.assetMgr().createAsset<lux::asset::MeshAsset>(std::make_unique<lux::rdesc::Mesh>());
        const auto id = a->id();
        fix.assetMgr().registerAsset(std::move(a));
        return id;
    }

    std::unique_ptr<lux::gameplay::RenderableSystem> makeMeshSystem(lux::bridgetest::HeadlessBridgeFixture& fix)
    {
        auto sys = std::make_unique<lux::gameplay::RenderableSystem>(
            fix.session(), fix.assetMgr(), fix.scene(), fix.view(), lux::gameplay::AssetLoadFn{});
        sys->setMeshStackOps(fix.meshStackOps());
        sys->addBridge(std::make_unique<lux::gameplay::InstanceBridge<MeshComponent>>());
        return sys;
    }

    // ── Scenario A: permanent (InvalidConfiguration) failure is not retried (G-05) ──
    void scenarioPermanentFailureNoRetry()
    {
        std::printf("-- G-05: permanent failure is not re-issued --\n");
        lux::bridgetest::HeadlessBridgeFixture fix;
        fix.registerMeshStackOps();
        fix.recorder().add_instance_status = lux::render::MeshInstanceCreateStatus::InvalidConfiguration;

        const auto mesh_id = makeMeshAsset(fix);
        auto sys = makeMeshSystem(fix);
        lux::meta::EntityRegistry reg;
        const auto e = reg.create();
        reg.emplace<MeshComponent>(e).mesh_asset_id = mesh_id;
        reg.emplace<WorldTransformComponent>(e);

        fix.beginFrame();
        sys->update(reg, 0.f);
        fix.roundTrip();   // upload reply → mesh handle cached
        check(fix.recorder().count("UploadMesh") == 1, "ensureMesh emits one UploadMesh");
        check(fix.recorder().count("AddMeshInstance") == 0, "no AddMeshInstance until the mesh is ready");

        sys->update(reg, 0.f);
        fix.roundTrip();   // add reply (FAILURE) → failed_ recorded, NOT live
        check(fix.recorder().count("AddMeshInstance") == 1, "a ready mesh emits one AddMeshInstance");
        check(fix.recorder().count("UploadMesh") == 1, "a cached mesh is not re-uploaded");

        sys->update(reg, 0.f);
        fix.roundTrip();
        sys->update(reg, 0.f);
        fix.roundTrip();
        check(fix.recorder().count("AddMeshInstance") == 1, "a permanent failure is not retried (G-05)");
        check(fix.recorder().count("RemoveMeshInstance") == 0, "a never-live instance is never removed");

        sys->beginShutdown();
        fix.roundTrip();
        check(!sys->hasPendingShutdownWork(), "no pending shutdown work after a failed create");
        sys->flushShutdownCleanup();
        fix.roundTrip();
        check(fix.recorder().count("RemoveMeshInstance") == 0, "shutdown removes nothing for a failed create");
    }

    // ── Scenario B: a DEFAULT/Unknown status carrying a valid object must NOT go live (P1-2) ──
    void scenarioDefaultReplyNotLive()
    {
        std::printf("-- P1-2: default (Unknown) status is not treated as Ok --\n");
        lux::bridgetest::HeadlessBridgeFixture fix;
        fix.registerMeshStackOps();
        // The real dispatch-failure shape: the server never typed-filled status (default
        // Unknown) yet a slot object is present. Force the object so the `!r.object` half
        // cannot mask the status check — the STATUS default alone must reject it.
        fix.recorder().add_instance_status         = lux::render::MeshInstanceCreateStatus::Unknown;
        fix.recorder().add_instance_object_on_failure = true;

        const auto mesh_id = makeMeshAsset(fix);
        auto sys = makeMeshSystem(fix);
        lux::meta::EntityRegistry reg;
        const auto e = reg.create();
        reg.emplace<MeshComponent>(e).mesh_asset_id = mesh_id;
        reg.emplace<WorldTransformComponent>(e);

        fix.beginFrame();
        sys->update(reg, 0.f);
        fix.roundTrip();   // upload → cached
        sys->update(reg, 0.f);
        fix.roundTrip();   // addMeshInstance → reply {Unknown, non-null object}
        check(fix.recorder().count("AddMeshInstance") == 1, "addMeshInstance issued once");
        check(fix.recorder().created_objects.size() == 1, "server handed back a valid object");

        // Prove the bridge did NOT go live: remove the entity and drive so reap runs.
        // A live instance would emit exactly one RemoveMeshInstance; a rejected one emits none.
        reg.destroy(e);
        sys->update(reg, 0.f);
        fix.roundTrip();
        check(fix.recorder().count("RemoveMeshInstance") == 0,
              "an Unknown-status reply never became live (P1-2; fails under old default-Ok)");

        sys->beginShutdown();
        fix.roundTrip();
        sys->flushShutdownCleanup();
        fix.roundTrip();
        check(fix.recorder().count("RemoveMeshInstance") == 0, "teardown removes nothing for a rejected create");
    }
}

int main()
{
    scenarioPermanentFailureNoRetry();
    scenarioDefaultReplyNotLive();
    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
